#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>

#include <lwip/sockets.h>

#include <unistd.h>

#ifdef ESP_PLATFORM

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "http_parser.h"
#include "esp32_port.h"
#include "esp_attr.h"
#define IRAM                        IRAM_ATTR

#define HK_LONGINT_F                "li"

#else

#include <FreeRTOS.h>
#include <task.h>
#include <espressif/esp_common.h>
#include <esplibs/libmain.h>
#include <sysparam.h>
#include <http-parser/http_parser.h>

#define HK_LONGINT_F                "i"

#endif

#include <cJSON_rsf.h>
#include <wolfssl/wolfcrypt/hash.h>
#include <wolfssl/wolfcrypt/coding.h>

#include <timers_helper.h>

#include "base64.h"
#include "crypto.h"
#include "pairing.h"
#include "storage.h"
#include "query_params.h"
#include "json.h"
#include "debug.h"
#include "port.h"

#include <homekit/homekit.h>
#include <homekit/characteristics.h>
#include <homekit/tlv.h>


#define PORT                                    (5556)

#ifndef HOMEKIT_MAX_CLIENTS_DEFAULT
#define HOMEKIT_MAX_CLIENTS_DEFAULT             (8)
#endif

#ifndef HOMEKIT_MIN_CLIENTS
#define HOMEKIT_MIN_CLIENTS                     (2)
#endif

#ifndef HOMEKIT_MIN_FREEHEAP
#define HOMEKIT_MIN_FREEHEAP                    (14848)
#endif

#ifndef HOMEKIT_NETWORK_FIRST_MIN_FREEHEAP
#define HOMEKIT_NETWORK_FIRST_MIN_FREEHEAP      (25600)
#endif

#ifndef HOMEKIT_NETWORK_MIN_FREEHEAP
#define HOMEKIT_NETWORK_MIN_FREEHEAP            (20480)
#endif

#ifndef HOMEKIT_NETWORK_PAUSE_COUNT
#define HOMEKIT_NETWORK_PAUSE_COUNT             (5)
#endif

#ifndef HOMEKIT_NETWORK_MIN_FREEHEAP_CRITIC
#define HOMEKIT_NETWORK_MIN_FREEHEAP_CRITIC     (16896)
#endif

#ifndef HOMEKIT_NETWORK_PAUSE_COUNT_CRITIC
#define HOMEKIT_NETWORK_PAUSE_COUNT_CRITIC      (10)
#endif

#ifdef HOMEKIT_DEBUG
#define TLV_DEBUG(values)                       tlv_debug(values)
#else
#define TLV_DEBUG(values)
#endif

#define HOMEKIT_DEBUG_LOG(message, ...)         DEBUG(message, ##__VA_ARGS__)
#define HOMEKIT_INFO(message, ...)              INFO(message, ##__VA_ARGS__)
#define HOMEKIT_ERROR(message, ...)             ERROR(message, ##__VA_ARGS__)

#define CLIENT_DEBUG(client, message, ...)      DEBUG("[%"HK_LONGINT_F"] " message, client->socket, ##__VA_ARGS__)
#define CLIENT_INFO(client, message, ...)       INFO("[%"HK_LONGINT_F"] " message, client->socket, ##__VA_ARGS__)
#define CLIENT_ERROR(client, message, ...)      ERROR("[%"HK_LONGINT_F"] " message, client->socket, ##__VA_ARGS__)

struct _client_context_t;
typedef struct _client_context_t client_context_t;

#ifdef HOMEKIT_NOTIFY_EVENT_ENABLE

#define HOMEKIT_NOTIFY_EVENT(server, event) \
  if ((server)->config->on_event) \
      (server)->config->on_event(event);

#else

#define HOMEKIT_NOTIFY_EVENT(server, event)

#endif


#define HOMEKIT_ENDPOINT_UNKNOWN                    (0)
#define HOMEKIT_ENDPOINT_PAIR_SETUP                 (1)
#define HOMEKIT_ENDPOINT_PAIR_VERIFY                (2)
#define HOMEKIT_ENDPOINT_IDENTIFY                   (3)
#define HOMEKIT_ENDPOINT_GET_ACCESSORIES            (4)
#define HOMEKIT_ENDPOINT_GET_CHARACTERISTICS        (5)
#define HOMEKIT_ENDPOINT_UPDATE_CHARACTERISTICS     (6)
#define HOMEKIT_ENDPOINT_PAIRINGS                   (7)
#define HOMEKIT_ENDPOINT_PREPARE                    (8)
#define HOMEKIT_ENDPOINT_RESOURCE                   (9)


typedef struct {
    Srp *srp;
    byte *public_key;
    size_t public_key_size;

    client_context_t *client;
} pairing_context_t;

typedef struct {
    byte *secret;
    size_t secret_size;
    byte *session_key;
    size_t session_key_size;
    byte *device_public_key;
    size_t device_public_key_size;
    byte *accessory_public_key;
    size_t accessory_public_key_size;
} pair_verify_context_t;

typedef struct _notification {
    homekit_characteristic_t* ch;
    struct _notification* next;
} notification_t;

#define BUFFER_DATA_SIZE        (HOMEKIT_JSON_BUFFER_SIZE)  // Used by JSON buffer too. Must be 2 bytes reserved for client_send_chunk() end

typedef struct {
    char *accessory_id;
    ed25519_key* accessory_key;
    
    homekit_server_config_t* config;
    
    pairing_context_t* pairing_context;
    
    client_context_t* clients;
    
    notification_t* notifications;
    
    int32_t listen_fd;
    int32_t max_fd;
    
    uint8_t client_count: 5;
    bool paired: 1;
    bool is_pairing: 1;
    bool pending_close: 1;
    
    json_stream json;
    
    byte data[BUFFER_DATA_SIZE + 16 + 2];   // Used by JSON buffer too. Must be 2 bytes reserved for client_send_chunk() end; there are 18.
    byte encrypted[BUFFER_DATA_SIZE + 16 + 2];
    
    fd_set fds;
} homekit_server_t;

static homekit_server_t *homekit_server = NULL;

struct _client_context_t {
    int32_t socket;
    query_param_t *endpoint_params;
    
    char *body;
    unsigned int body_length: 16;
    byte permissions;
    uint8_t endpoint: 4;
    bool encrypted: 1;
    bool disconnect: 1;
    
    http_parser *parser;

    int32_t pairing_id;
    
    byte read_key[32];
    byte write_key[32];
    int32_t count_reads;
    int32_t count_writes;
    
    pair_verify_context_t *verify_context;

    struct _client_context_t *next;
};

#ifdef HOMEKIT_GET_CLIENTS_INFO
int32_t homekit_get_unique_client_ipaddr() {
    if (homekit_server && homekit_server->client_count == 1) {
        struct sockaddr_in addr;
        socklen_t addr_len = sizeof(addr);
        if (getpeername(homekit_server->clients->socket, (struct sockaddr*) &addr, &addr_len) == 0) {
            return ((((const uint8_t*) &addr.sin_addr.s_addr)[2]) * 1000) + ((const uint8_t*) &addr.sin_addr.s_addr)[3];
        }
    }
    
    return -1;
}

int homekit_get_client_count() {
    if (homekit_server) {
        return homekit_server->client_count;
    }
    
    return -1;
}
#endif

void client_context_free(client_context_t *c);
void pairing_context_free(pairing_context_t *context);
void homekit_server_on_reset(client_context_t *context);
int client_send_chunk(byte *data, size_t size, void *arg);

#ifdef HOMEKIT_CHANGE_MAX_CLIENTS
void homekit_set_max_clients(const unsigned int clients) {
    if (homekit_server && homekit_server->config && clients > 0 && clients <= 32) {
        homekit_server->config->max_clients = clients;
    }
}
#endif // HOMEKIT_CHANGE_MAX_CLIENTS

homekit_server_t *server_new() {
    homekit_server_t* homekit_server = calloc(1, sizeof(homekit_server_t));
    
    FD_ZERO(&homekit_server->fds);
    
    homekit_server->json.buffer = homekit_server->data;
    homekit_server->json.on_flush = client_send_chunk;
    
    return homekit_server;
}

/*
void server_free() {
    if (homekit_server->accessory_id) {
        free(homekit_server->accessory_id);
    }

    if (homekit_server->accessory_key) {
        crypto_ed25519_free(homekit_server->accessory_key);
    }

    if (homekit_server->pairing_context) {
        pairing_context_free(homekit_server->pairing_context);
    }

    if (homekit_server->clients) {
        client_context_t *client = homekit_server->clients;
        while (client) {
            client_context_t *next = client->next;
            client_context_free(client);
            client = next;
        }
    }

    free(homekit_server);
    homekit_server = NULL;
}
*/

void tlv_debug(const tlv_values_t *values) {
    HOMEKIT_DEBUG_LOG("Got following TLV Values:");
    for (tlv_t *t=values->head; t; t=t->next) {
        char *escaped_payload = binary_to_string(t->value, t->size);
        HOMEKIT_DEBUG_LOG("Type 0x%02X, Length: %d, Value: %s", t->type, t->size, escaped_payload);
        free(escaped_payload);
    }
}


typedef enum {
  TLVError_Unknown = 1,         // Generic error to handle unexpected errors
  TLVError_Authentication = 2,  // Setup code or signature verification failed
  TLVError_Backoff = 3,         // Client must look at the retry delay TLV item and
                                // wait that many seconds before retrying
  TLVError_MaxPeers = 4,        // Server cannot accept any more pairings
  TLVError_MaxTries = 5,        // Server reached its maximum number of
                                // authentication attempts
  TLVError_Unavailable = 6,     // Server pairing method is unavailable
  TLVError_Busy = 7,            // Server is busy and cannot accept a pairing
                                // request at this time
} TLVError;


typedef enum {
    // This specifies a success for the request
    HAPStatus_Success = 0,
    // Request denied due to insufficient privileges
    HAPStatus_InsufficientPrivileges = -70401,
    // Unable to communicate with requested services,
    // e.g. the power to the accessory was turned off
    HAPStatus_NoAccessoryConnection = -70402,
    // Resource is busy, try again
    HAPStatus_ResourceBusy = -70403,
    // Connot write to read only characteristic
    HAPStatus_ReadOnly = -70404,
    // Cannot read from a write only characteristic
    HAPStatus_WriteOnly = -70405,
    // Notification is not supported for characteristic
    HAPStatus_NotificationsUnsupported = -70406,
    // Out of resources to process request
    HAPStatus_OutOfResources = -70407,
    // Operation timed out
    HAPStatus_Timeout = -70408,
    // Resource does not exist
    HAPStatus_NoResource = -70409,
    // Accessory received an invalid value in a write request
    HAPStatus_InvalidValue = -70410,
    // Insufficient Authorization
    HAPStatus_InsufficientAuthorization = -70411,
} HAPStatus;


pair_verify_context_t *pair_verify_context_new() {
    pair_verify_context_t *context = calloc(1, sizeof(pair_verify_context_t));
    
    return context;
}

void pair_verify_context_free(pair_verify_context_t **context) {
    if ((*context)->secret)
        free((*context)->secret);

    if ((*context)->session_key)
        free((*context)->session_key);

    if ((*context)->device_public_key)
        free((*context)->device_public_key);

    if ((*context)->accessory_public_key)
        free((*context)->accessory_public_key);

    free(*context);
    *context = NULL;
}


client_context_t *client_context_new() {
    client_context_t *c = calloc(1, sizeof(client_context_t));
    if (c) {
        c->pairing_id = -1;
        
        c->parser = malloc(sizeof(*c->parser));
        http_parser_init(c->parser, HTTP_REQUEST);
        c->parser->data = c;
    }
    
    return c;
}


void client_context_free(client_context_t *c) {
    if (c->verify_context)
        pair_verify_context_free(&c->verify_context);
    
    if (c->endpoint_params)
        query_params_free(c->endpoint_params);
    
    if (c->parser)
        free(c->parser);
    
    if (c->body)
        free(c->body);
    
    free(c);
}


pairing_context_t *pairing_context_new() {
    pairing_context_t *context = calloc(1, sizeof(pairing_context_t));
    
    context->srp = crypto_srp_new();
    
    return context;
}

void pairing_context_free(pairing_context_t *context) {
    if (context->srp) {
        crypto_srp_free(context->srp);
    }
    
    if (context->public_key) {
        free(context->public_key);
    }
    
    free(context);
}

static int homekit_low_dram() {
    const uint_fast32_t free_heap = xPortGetFreeHeapSize();
    if (free_heap < HOMEKIT_MIN_FREEHEAP) {
        HOMEKIT_ERROR("DRAM Free HEAP %"HK_LONGINT_F, free_heap);
        return true;
    }
    
    return false;
}

void homekit_disconnect_client(client_context_t* context) {
    context->disconnect = true;
    homekit_server->pending_close = true;
}

void IRAM homekit_remove_oldest_client() {
    if (homekit_server && homekit_server->client_count > HOMEKIT_MIN_CLIENTS) {
        client_context_t* context = homekit_server->clients;
        while (context) {
            if (!context->next) {
                CLIENT_INFO(context, "Closing oldest");
                homekit_disconnect_client(context);
            }
            
            context = context->next;
        }
    }
}


typedef enum {
    characteristic_format_type   = (1 << 1),
    characteristic_format_meta   = (1 << 2),
    characteristic_format_perms  = (1 << 3),
    characteristic_format_events = (1 << 4),
} characteristic_format_t;


void write_characteristic_json(json_stream *json, client_context_t *client, const homekit_characteristic_t *ch, characteristic_format_t format, const homekit_value_t *value, const uint16_t override_aid) {
    json_string(json, "aid");
    if (override_aid > 0) {
        json_integer(json, override_aid);
    } else {
        json_integer(json, ch->service->accessory->id);
    }
    
    json_string(json, "iid");
    json_integer(json, ch->id);

    if (format & characteristic_format_type) {
        json_string(json, "type"); json_string(json, ch->type);
    }

    if (format & characteristic_format_perms) {
        json_string(json, "perms"); json_array_start(json);
        if (ch->permissions & HOMEKIT_PERMISSIONS_PAIRED_READ)
            json_string(json, "pr");
        if (ch->permissions & HOMEKIT_PERMISSIONS_PAIRED_WRITE)
            json_string(json, "pw");
        if (ch->permissions & HOMEKIT_PERMISSIONS_NOTIFY)
            json_string(json, "ev");
        if (ch->permissions & HOMEKIT_PERMISSIONS_ADDITIONAL_AUTHORIZATION)
            json_string(json, "aa");
        if (ch->permissions & HOMEKIT_PERMISSIONS_TIMED_WRITE)
            json_string(json, "tw");
        if (ch->permissions & HOMEKIT_PERMISSIONS_HIDDEN)
            json_string(json, "hd");
        json_array_end(json);
    }

    if ((format & characteristic_format_events) && (ch->permissions & HOMEKIT_PERMISSIONS_NOTIFY)) {
        int events = homekit_characteristic_has_notify_subscription(ch, client);
        json_string(json, "ev");
        json_boolean(json, events);
    }

    if (format & characteristic_format_meta) {
        if (ch->description) {
            json_string(json, "description"); json_string(json, ch->description);
        }

        const char *format_str = NULL;
        switch(ch->format) {
            case HOMEKIT_FORMAT_BOOL:      format_str = "bool"; break;
            case HOMEKIT_FORMAT_UINT8:     format_str = "uint8"; break;
            case HOMEKIT_FORMAT_UINT16:    format_str = "uint16"; break;
            case HOMEKIT_FORMAT_UINT32:    format_str = "uint32"; break;
            case HOMEKIT_FORMAT_UINT64:    format_str = "uint64"; break;
            case HOMEKIT_FORMAT_INT:       format_str = "int"; break;
            case HOMEKIT_FORMAT_FLOAT:     format_str = "float"; break;
            case HOMEKIT_FORMAT_STRING:    format_str = "string"; break;
            case HOMEKIT_FORMAT_TLV:       format_str = "tlv8"; break;
            case HOMEKIT_FORMAT_DATA:      format_str = "data"; break;
        }
        if (format_str) {
            json_string(json, "format"); json_string(json, format_str);
        }

        const char *unit_str = NULL;
        switch(ch->unit) {
            case HOMEKIT_UNIT_NONE:        break;
            case HOMEKIT_UNIT_CELSIUS:     unit_str = "celsius"; break;
            case HOMEKIT_UNIT_PERCENTAGE:  unit_str = "percentage"; break;
            case HOMEKIT_UNIT_ARCDEGREES:  unit_str = "arcdegrees"; break;
            case HOMEKIT_UNIT_LUX:         unit_str = "lux"; break;
            case HOMEKIT_UNIT_SECONDS:     unit_str = "seconds"; break;
        }
        if (unit_str) {
            json_string(json, "unit"); json_string(json, unit_str);
        }

        if (ch->min_value) {
            json_string(json, "minValue"); json_float(json, *ch->min_value);
        }

        if (ch->max_value) {
            json_string(json, "maxValue"); json_float(json, *ch->max_value);
        }

        if (ch->min_step) {
            json_string(json, "minStep"); json_float(json, *ch->min_step);
        }

#ifndef HOMEKIT_DISABLE_MAXLEN_CHECK
        if (ch->max_len) {
            json_string(json, "maxLen"); json_integer(json, *ch->max_len);
        }

        if (ch->max_data_len) {
            json_string(json, "maxDataLen"); json_integer(json, *ch->max_data_len);
        }
#endif //HOMEKIT_DISABLE_MAXLEN_CHECK

        if (ch->valid_values.count) {
            json_string(json, "valid-values"); json_array_start(json);

            for (unsigned int i = 0; i < ch->valid_values.count; i++) {
                json_integer(json, ch->valid_values.values[i]);
            }

            json_array_end(json);
        }

#ifndef HOMEKIT_DISABLE_VALUE_RANGES
        if (ch->valid_values_ranges.count) {
            json_string(json, "valid-values-range"); json_array_start(json);

            for (unsigned int i = 0; i < ch->valid_values_ranges.count; i++) {
                json_array_start(json);

                json_integer(json, ch->valid_values_ranges.ranges[i].start);
                json_integer(json, ch->valid_values_ranges.ranges[i].end);

                json_array_end(json);
            }

            json_array_end(json);
        }
#endif //HOMEKIT_DISABLE_VALUE_RANGES
        
    }
    
    if (ch->permissions & HOMEKIT_PERMISSIONS_PAIRED_READ) {
        homekit_value_t v = value ? *value : ch->getter_ex ? ch->getter_ex(ch) : ch->value;
        
        if (v.is_null) {
            json_string(json, "value"); json_null(json);
        } else if (v.format != ch->format) {
            HOMEKIT_ERROR("Ch value format is different from ch format");
        } else {
            switch(v.format) {
                case HOMEKIT_FORMAT_BOOL: {
                    json_string(json, "value"); json_boolean(json, v.bool_value);
                    break;
                }
                case HOMEKIT_FORMAT_UINT8:
                case HOMEKIT_FORMAT_UINT16:
                case HOMEKIT_FORMAT_UINT32:
                case HOMEKIT_FORMAT_UINT64:
                case HOMEKIT_FORMAT_INT: {
                    if (ch->max_value) {
                        int max_value = (int) *ch->max_value;
                        if (v.int_value > max_value) {
                            v.int_value = max_value;
                        }
                    }
                    
                    if (ch->min_value) {
                        int min_value = (int) *ch->min_value;
                        if (v.int_value < min_value) {
                            v.int_value = min_value;
                        }
                    }
                    
                    json_string(json, "value"); json_integer(json, v.int_value);
                    break;
                }
                case HOMEKIT_FORMAT_FLOAT: {
                    // Check for 'nan' float value
                    if (v.float_value != v.float_value) {
                        v.float_value = 0;
                    }
                    
                    if (ch->max_value) {
                        int max_value = (int) *ch->max_value;
                        if (v.float_value > max_value) {
                            v.float_value = max_value;
                        }
                    }
                    
                    if (ch->min_value) {
                        int min_value = (int) *ch->min_value;
                        if (v.float_value < min_value) {
                            v.float_value = min_value;
                        }
                    }
                    
                    json_string(json, "value"); json_float(json, v.float_value);
                    break;
                }
                case HOMEKIT_FORMAT_STRING: {
                    json_string(json, "value"); json_string(json, v.string_value);
                    break;
                }
                case HOMEKIT_FORMAT_TLV: {
                    json_string(json, "value");
                    if (!v.tlv_values) {
                        json_string(json, "");
                    } else {
                        size_t tlv_size = 0;
                        tlv_format(v.tlv_values, NULL, &tlv_size);
                        if (tlv_size == 0) {
                            json_string(json, "");
                        } else {
                            byte *tlv_data = malloc(tlv_size);
                            tlv_format(v.tlv_values, tlv_data, &tlv_size);

                            size_t encoded_tlv_size = base64_encoded_size(tlv_data, tlv_size);
                            byte *encoded_tlv_data = malloc(encoded_tlv_size + 1);
                            base64_encode(tlv_data, tlv_size, encoded_tlv_data);
                            encoded_tlv_data[encoded_tlv_size] = 0;

                            json_string(json, (char*) encoded_tlv_data);

                            free(encoded_tlv_data);
                            free(tlv_data);
                        }
                    }
                    break;
                }
                case HOMEKIT_FORMAT_DATA:
                    json_string(json, "value");
                    if (!v.data_value || v.data_size == 0) {
                        json_string(json, "");
                    } else {
                        size_t encoded_data_size = base64_encoded_size(v.data_value, v.data_size);
                        byte* encoded_data = malloc(encoded_data_size + 1);
                        if (!encoded_data) {
                            CLIENT_ERROR(client, "Allocate %d bytes for encoding", encoded_data_size + 1);
                            json_string(json, "");
                            break;
                        }
                        base64_encode(v.data_value, v.data_size, encoded_data);
                        encoded_data[encoded_data_size] = 0;

                        json_string(json, (char*) encoded_data);
                        
                        free(encoded_data);
                    }
                    break;
            }
        }

        if (!value && ch->getter_ex) {
            // called getter to get value, need to free it
            homekit_value_destruct(&v);
        }
    }
}

static void network_delay(const uint32_t free_heap) {
    if (free_heap < HOMEKIT_NETWORK_MIN_FREEHEAP) {
        unsigned int max_count = HOMEKIT_NETWORK_PAUSE_COUNT;
        if (free_heap < HOMEKIT_NETWORK_MIN_FREEHEAP_CRITIC) {
            max_count = HOMEKIT_NETWORK_PAUSE_COUNT_CRITIC;
        }
        
        unsigned int count = 0;
        while (free_heap > xPortGetFreeHeapSize() && count < max_count) {
            count++;
            //HOMEKIT_INFO("Net Delay %i", count);
            vTaskDelay(1);
        }
        
    } else if (free_heap < HOMEKIT_NETWORK_FIRST_MIN_FREEHEAP) {
        vTaskDelay(1);
    }
}

int client_send_encrypted(client_context_t *context, byte *payload, size_t size) {
    if (!context || !context->encrypted) {
        return -1;
    }

    byte nonce[12];
    memset(nonce, 0, sizeof(nonce));
    
    size_t payload_offset = 0;
    
    while (payload_offset < size) {
        size_t chunk_size = size - payload_offset;
        if (chunk_size > sizeof(homekit_server->encrypted) - 16 - 2) {
            chunk_size = sizeof(homekit_server->encrypted) - 16 - 2;
        }
        
        byte aead[2] = {chunk_size % 256, chunk_size / 256};

        memcpy(homekit_server->encrypted, aead, 2);

        byte i = 4;
        int x = context->count_reads++;
        while (x) {
            nonce[i++] = x % 256;
            x /= 256;
        }
        
        size_t available = sizeof(homekit_server->encrypted) - 2;
        int r = crypto_chacha20poly1305_encrypt(
            context->read_key, nonce, aead, 2,
            payload + payload_offset, chunk_size,
            homekit_server->encrypted + 2, &available
        );
        if (r) {
            CLIENT_ERROR(context, "Enc payload (%d)", r);
            return -1;
        }
        
        payload_offset += chunk_size;
        
        const uint_fast32_t free_heap = xPortGetFreeHeapSize();
        
        r = write(context->socket, homekit_server->encrypted, available + 2);
        
        if (r < 0) {
            CLIENT_ERROR(context, "Payload");
            return r;
        }
        
        network_delay(free_heap);
    }

    return 0;
}


int client_decrypt(client_context_t *context, byte *payload, size_t payload_size, byte *decrypted, size_t *decrypted_size) {
    if (!context || !context->encrypted)
        return -1;

    const size_t block_size = 1024 + 16 + 2;
    size_t required_decrypted_size = payload_size / block_size * 1024;
    if (payload_size % block_size > 0)
        required_decrypted_size += payload_size % block_size - 16 - 2;
    
    if (*decrypted_size < required_decrypted_size) {
        *decrypted_size = required_decrypted_size;
        return -2;
    }

    *decrypted_size = required_decrypted_size;

    byte nonce[12];
    memset(nonce, 0, sizeof(nonce));

    size_t payload_offset = 0;
    size_t decrypted_offset = 0;

    while (payload_offset < payload_size) {
        size_t chunk_size = payload[payload_offset] + payload[payload_offset + 1] * 256;
        if (chunk_size+18 > payload_size-payload_offset) {
            // Unfinished chunk
            break;
        }

        byte i = 4;
        size_t x = context->count_writes++;
        while (x) {
            nonce[i++] = x % 256;
            x /= 256;
        }

        size_t decrypted_len = *decrypted_size - decrypted_offset;
        int r = crypto_chacha20poly1305_decrypt(
            context->write_key, nonce, payload + payload_offset, 2,
            payload + payload_offset + 2, chunk_size + 16,
            decrypted, &decrypted_len
        );
        if (r) {
            CLIENT_ERROR(context, "Decrypt payload (%d)", r);
            return -1;
        }

        decrypted_offset += decrypted_len;
        payload_offset += chunk_size + 18;
    }

    return payload_offset;
}


void homekit_setup_mdns();


int client_send(client_context_t *context, byte *data, size_t data_size) {
#if HOMEKIT_DEBUG
    if (data_size < 4096) {
        char *payload = binary_to_string(data, data_size);
        CLIENT_DEBUG(context, "Sending payload: %s", payload);
        free(payload);
    }
#endif

    int r = -1;
    
    if (context->encrypted) {
        r = client_send_encrypted(context, data, data_size);
    } else {
        const uint_fast32_t free_heap = xPortGetFreeHeapSize();
        
        r = write(context->socket, data, data_size);
        
        network_delay(free_heap);
    }
    
    if (r < 0) {
        CLIENT_ERROR(context, "Response");
        return r;
    }
    
    return 0;
}


int client_send_chunk(byte *data, size_t size, void *arg) {
    client_context_t* context = arg;
    
    byte header[9];
    header[0] = 0;
    int header_size = snprintf((char*) header, sizeof(header), "%x\r\n", size);
    
    int r = client_send(context, header, header_size);
    
    if (r == 0) {
        if (size > 0) {
            data[size] = '\r';
            data[size + 1] = '\n';
            r = client_send(context, data, size + 2);
        } else {
            byte end[2] = { '\r', '\n' };
            r = client_send(context, end, sizeof(end));
        }
    }
    
    return r;
    
    /*
    size_t payload_size = size + 8;
    byte *payload = malloc(payload_size);

    int offset = snprintf((char *)payload, payload_size, "%x\r\n", size);
    memcpy(payload + offset, data, size);
    payload[offset + size] = '\r';
    payload[offset + size + 1] = '\n';

    client_send(context, payload, offset + size + 2);

    free(payload);
     */
}

int send_200_response(client_context_t* context) {
    byte response[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/hap+json\r\n"
        "Transfer-Encoding: chunked\r\n\r\n";
    return client_send(context, response, sizeof(response) - 1);
}

void send_204_response(client_context_t* context) {
    byte response[] = "HTTP/1.1 204 No Content\r\n\r\n";
    client_send(context, response, sizeof(response) - 1);
}

int send_207_response(client_context_t* context) {
    byte response[] =
        "HTTP/1.1 207 Multi-Status\r\n"
        "Content-Type: application/hap+json\r\n"
        "Transfer-Encoding: chunked\r\n\r\n";
    return client_send(context, response, sizeof(response) - 1);
}

void send_404_response(client_context_t* context) {
    byte response[] =
        "HTTP/1.1 404 Not Found\r\n"
        "Content-Length: 0\r\n\r\n";
    client_send(context, response, sizeof(response) - 1);
}

void send_tlv_response(client_context_t *context, tlv_values_t *values) {
    CLIENT_DEBUG(context, "Sending TLV response");
    TLV_DEBUG(values);

    size_t payload_size = 0;
    tlv_format(values, NULL, &payload_size);

    byte *payload = malloc(payload_size);
    
    if (!payload) {
        CLIENT_ERROR(context, "TLV payload DRAM");
        tlv_free(values);
        homekit_remove_oldest_client();
        return;
    }
    
    int r = tlv_format(values, payload, &payload_size);
    if (r) {
        CLIENT_ERROR(context, "TLV format (%d)", r);
        free(payload);
        return;
    }
    
    tlv_free(values);
    
    const char http_headers[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/pairing+tlv8\r\n"
        "Content-Length: %d\r\n\r\n";
    
    size_t response_size = strlen(http_headers) + payload_size + 32;
    char *response = malloc(response_size);
    
    if (!response) {
        CLIENT_ERROR(context, "TLV response DRAM");
        free(payload);
        homekit_remove_oldest_client();
        return;
    }
    
    size_t response_len = snprintf(response, response_size, http_headers, payload_size);

    if (response_size - response_len < payload_size + 1) {
        CLIENT_ERROR(context, "Buffer size %d: headers took %d, payload size %d", response_size, response_len, payload_size);
        free(response);
        free(payload);
        return;
    }
    memcpy(response + response_len, payload, payload_size);
    free(payload);
    
    response_len += payload_size;
    
    client_send(context, (byte*) response, response_len);

    free(response);
}

void send_tlv_error_response(client_context_t *context, int state, TLVError error) {
    tlv_values_t *response = tlv_new();
    tlv_add_integer_value(response, TLVType_State, 1, state);
    tlv_add_integer_value(response, TLVType_Error, 1, error);

    send_tlv_response(context, response);
}

void send_json_response(client_context_t *context, int status_code, byte *payload, size_t payload_size) {
    CLIENT_DEBUG(context, "Sending JSON response");
    DEBUG_HEAP();

    CLIENT_DEBUG(context, "Payload: %s", payload);

    const char *status_text = "OK";
    switch (status_code) {
        case 204: status_text = "No Content"; break;
        case 207: status_text = "Multi-Status"; break;
        case 400: status_text = "Bad Request"; break;
        case 404: status_text = "Not Found"; break;
        case 422: status_text = "Unprocessable Entity"; break;
        case 500: status_text = "Internal Server Error"; break;
        case 503: status_text = "Service Unavailable"; break;
    }

    const char http_headers[] =
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/hap+json\r\n"
        "Content-Length: %d\r\n\r\n";
    
    size_t response_size = strlen(http_headers) + payload_size + strlen(status_text) + 32;
    char *response = malloc(response_size);
    if (!response) {
        CLIENT_ERROR(context, "Buffer of %d DRAM", response_size);
        homekit_remove_oldest_client();
        return;
    }
    size_t response_len = snprintf(response, response_size, http_headers, status_code, status_text, payload_size);

    if (response_size - response_len < payload_size + 1) {
        CLIENT_ERROR(context, "Buffer size %d: headers took %d, payload size %d", response_size, response_len, payload_size);
        free(response);
        return;
    }
    memcpy(response+response_len, payload, payload_size);
    response_len += payload_size;
    
#ifdef HOMEKIT_DEBUG
    response[response_len] = 0;  // required for debug output
#endif
    CLIENT_DEBUG(context, "Sending HTTP response: %s", response);

    client_send(context, (byte *)response, response_len);

    free(response);
}


void send_json_error_response(client_context_t *context, int status_code, HAPStatus status) {
    byte buffer[32];
    int size = snprintf((char *)buffer, sizeof(buffer), "{\"status\": %d}", status);

    send_json_response(context, status_code, buffer, size);
}

/*
static client_context_t *current_client_context = NULL;

homekit_client_id_t homekit_get_client_id() {
    return (homekit_client_id_t)current_client_context;
}

bool homekit_client_is_admin() {
    if (!current_client_context)
        return false;

    return current_client_context->permissions & pairing_permissions_admin;
}

int homekit_client_send(unsigned char *data, size_t size) {
    if (!current_client_context)
        return -1;

    client_send(current_client_context, data, size);

    return 0;
}
*/

void homekit_server_on_identify(client_context_t *context) {
    CLIENT_INFO(context, "Identify");
    DEBUG_HEAP();

    if (homekit_server->paired) {
        // Already paired
        send_json_error_response(context, 400, HAPStatus_InsufficientPrivileges);
        return;
    }

    send_204_response(context);

    homekit_accessory_t *accessory =
        homekit_accessory_by_id(homekit_server->config->accessories, 1);
    if (!accessory) {
        return;
    }

    homekit_service_t *accessory_info =
        homekit_service_by_type(accessory, HOMEKIT_SERVICE_ACCESSORY_INFORMATION);
    if (!accessory_info) {
        return;
    }

    homekit_characteristic_t *ch_identify =
        homekit_service_characteristic_by_type(accessory_info, HOMEKIT_CHARACTERISTIC_IDENTIFY);
    if (!ch_identify) {
        return;
    }

    if (ch_identify->setter_ex) {
        ch_identify->setter_ex(ch_identify, HOMEKIT_BOOL(true));
    }
}

void homekit_server_on_pair_setup(client_context_t *context, const byte *data, size_t size) {
    homekit_server->is_pairing = true;
    
    HOMEKIT_DEBUG_LOG("Pair Setup");
    DEBUG_HEAP();

    tlv_values_t *message = tlv_new();
    if (tlv_parse(data, size, message)) {
        CLIENT_ERROR(context, "TLV");
        tlv_free(message);
        send_tlv_error_response(context, 2, TLVError_Unknown);
        homekit_server->is_pairing = false;
        return;
    }
    
    TLV_DEBUG(message);
    
#ifdef HOMEKIT_OVERCLOCK_PAIR_SETUP
    sdk_system_overclock();
    HOMEKIT_DEBUG_LOG("CPU overclock");
#endif // HOMEKIT_OVERCLOCK_PAIR_SETUP
    
    switch(tlv_get_integer_value(message, TLVType_State, -1)) {
        case 1: {
            CLIENT_INFO(context, "Pairing 1/3");
            DEBUG_HEAP();
            if (homekit_server->paired) {
                CLIENT_INFO(context, "Already paired");
                send_tlv_error_response(context, 2, TLVError_Unavailable);
                break;
            }

            if (homekit_server->pairing_context) {
                if (homekit_server->pairing_context->client != context) {
                    CLIENT_INFO(context, "Another pairing in progress");
                    send_tlv_error_response(context, 2, TLVError_Busy);
                    break;
                }
            } else {
                homekit_server->pairing_context = pairing_context_new();
                homekit_server->pairing_context->client = context;
            }
            
            CLIENT_DEBUG(context, "Initializing crypto");
            DEBUG_HEAP();
            
            crypto_srp_init(
                homekit_server->pairing_context->srp,
                "Pair-Setup", "021-82-017"
            );
            
            if (homekit_server->pairing_context->public_key) {
                free(homekit_server->pairing_context->public_key);
                homekit_server->pairing_context->public_key = NULL;
            }
            homekit_server->pairing_context->public_key_size = 0;
            crypto_srp_get_public_key(homekit_server->pairing_context->srp, NULL, &homekit_server->pairing_context->public_key_size);

            homekit_server->pairing_context->public_key = malloc(homekit_server->pairing_context->public_key_size);
            int r = crypto_srp_get_public_key(homekit_server->pairing_context->srp, homekit_server->pairing_context->public_key, &homekit_server->pairing_context->public_key_size);
            if (r) {
                CLIENT_ERROR(context, "SPR key (%d). DRAM?", r);

                pairing_context_free(homekit_server->pairing_context);
                homekit_server->pairing_context = NULL;

                send_tlv_error_response(context, 2, TLVError_Unknown);
                break;
            }
            
            size_t salt_size = 16;
            //size_t salt_size = 0;
            //crypto_srp_get_salt(homekit_server->pairing_context->srp, NULL, &salt_size);
            
            byte *salt = malloc(salt_size);
            r = crypto_srp_get_salt(homekit_server->pairing_context->srp, salt, &salt_size);
            if (r) {
                CLIENT_ERROR(context, "Get salt (%d)", r);

                free(salt);
                pairing_context_free(homekit_server->pairing_context);
                homekit_server->pairing_context = NULL;

                send_tlv_error_response(context, 2, TLVError_Unknown);
                break;
            }
            
            tlv_values_t *response = tlv_new();
            tlv_add_integer_value(response, TLVType_State, 1, 2);
            tlv_add_value(response, TLVType_PublicKey, homekit_server->pairing_context->public_key, homekit_server->pairing_context->public_key_size);
            tlv_add_value(response, TLVType_Salt, salt, salt_size);
            free(salt);
            
            send_tlv_response(context, response);
            
            CLIENT_INFO(context, "Done 1/3");
            break;
        }
        
        case 3: {
            CLIENT_INFO(context, "Pairing 2/3");
            DEBUG_HEAP();
            tlv_t *device_public_key = tlv_get_value(message, TLVType_PublicKey);
            if (!device_public_key) {
                CLIENT_ERROR(context, "No pub key");
                send_tlv_error_response(context, 4, TLVError_Authentication);
                break;
            }

            tlv_t *proof = tlv_get_value(message, TLVType_Proof);
            if (!proof) {
                CLIENT_ERROR(context, "No proof");
                send_tlv_error_response(context, 4, TLVError_Authentication);
                break;
            }

#ifndef ESP_PLATFORM
            unsigned int low_mdns_buffer = false;
            if (xPortGetFreeHeapSize() < 25000) {
                homekit_mdns_buffer_set(500);
                low_mdns_buffer = true;
            }
#endif
            
            CLIENT_DEBUG(context, "Computing pairing code (SRP shared secret)");
            DEBUG_HEAP();
            int r = crypto_srp_compute_key(
                homekit_server->pairing_context->srp,
                device_public_key->value, device_public_key->size,
                homekit_server->pairing_context->public_key,
                homekit_server->pairing_context->public_key_size
            );
            
#ifndef ESP_PLATFORM
            if (low_mdns_buffer) {
                homekit_mdns_buffer_set(0);
            }
#endif
            
            if (r) {
                CLIENT_ERROR(context, "Pairing code (%d). DRAM?", r);
                send_tlv_error_response(context, 4, TLVError_Authentication);
                pairing_context_free(homekit_server->pairing_context);
                homekit_server->pairing_context = NULL;
                break;
            }
            
            free(homekit_server->pairing_context->public_key);
            homekit_server->pairing_context->public_key = NULL;
            homekit_server->pairing_context->public_key_size = 0;

            CLIENT_DEBUG(context, "Verifying peer's proof");
            DEBUG_HEAP();
            
            r = crypto_srp_verify(homekit_server->pairing_context->srp, proof->value, proof->size);
            if (r) {
                CLIENT_ERROR(context, "Verify peer's proof (%d)", r);
                send_tlv_error_response(context, 4, TLVError_Authentication);
                break;
            }

            CLIENT_DEBUG(context, "Generating own proof");
            size_t server_proof_size = 0;
            crypto_srp_get_proof(homekit_server->pairing_context->srp, NULL, &server_proof_size);
            
            byte *server_proof = malloc(server_proof_size);
            crypto_srp_get_proof(homekit_server->pairing_context->srp, server_proof, &server_proof_size);
            
            tlv_values_t *response = tlv_new();
            tlv_add_integer_value(response, TLVType_State, 1, 4);
            tlv_add_value(response, TLVType_Proof, server_proof, server_proof_size);
            free(server_proof);
            
            send_tlv_response(context, response);
            
            CLIENT_INFO(context, "Done 2/3");
            break;
        }
            
        case 5: {
            CLIENT_INFO(context, "Pairing 3/3");
            DEBUG_HEAP();
            
            int r;
            
            byte shared_secret[HKDF_HASH_SIZE];
            size_t shared_secret_size = sizeof(shared_secret);
            
            CLIENT_DEBUG(context, "Calculating shared secret");
            const char salt1[] = "Pair-Setup-Encrypt-Salt";
            const char info1[] = "Pair-Setup-Encrypt-Info";
            r = crypto_srp_hkdf(
                homekit_server->pairing_context->srp,
                (byte *)salt1, sizeof(salt1)-1,
                (byte *)info1, sizeof(info1)-1,
                shared_secret, &shared_secret_size
            );
            if (r) {
                CLIENT_ERROR(context, "Generate shared secret (%d)", r);
                send_tlv_error_response(context, 6, TLVError_Authentication);
                break;
            }

            tlv_t *tlv_encrypted_data = tlv_get_value(message, TLVType_EncryptedData);
            if (!tlv_encrypted_data) {
                CLIENT_ERROR(context, "No encrypted data");
                send_tlv_error_response(context, 6, TLVError_Authentication);
                break;
            }

            CLIENT_DEBUG(context, "Decrypting payload");
            size_t decrypted_data_size = 0;
            crypto_chacha20poly1305_decrypt(
                shared_secret, (byte *)"\x0\x0\x0\x0PS-Msg05", NULL, 0,
                tlv_encrypted_data->value, tlv_encrypted_data->size,
                NULL, &decrypted_data_size
            );
            
            byte *decrypted_data = malloc(decrypted_data_size);
            if (decrypted_data) {
                r = crypto_chacha20poly1305_decrypt(
                        shared_secret, (byte *)"\x0\x0\x0\x0PS-Msg05", NULL, 0,
                        tlv_encrypted_data->value, tlv_encrypted_data->size,
                        decrypted_data, &decrypted_data_size
                    );
            } else {
                r = -100;
            }
            
            if (r) {
                CLIENT_ERROR(context, "Decrypt data (%d)", r);
                
                if (decrypted_data) {
                    free(decrypted_data);
                }
                
                send_tlv_error_response(context, 6, TLVError_Authentication);
                break;
            }

            tlv_values_t *decrypted_message = tlv_new();
            r = tlv_parse(decrypted_data, decrypted_data_size, decrypted_message);
            if (r) {
                CLIENT_ERROR(context, "Decrypt TLV (%d)", r);

                tlv_free(decrypted_message);
                free(decrypted_data);

                send_tlv_error_response(context, 6, TLVError_Authentication);
                break;
            }

            free(decrypted_data);

            tlv_t *tlv_device_id = tlv_get_value(decrypted_message, TLVType_Identifier);
            if (!tlv_device_id) {
                CLIENT_ERROR(context, "No id");

                tlv_free(decrypted_message);

                send_tlv_error_response(context, 6, TLVError_Authentication);
                break;
            }

            tlv_t *tlv_device_public_key = tlv_get_value(decrypted_message, TLVType_PublicKey);
            if (!tlv_device_public_key) {
                CLIENT_ERROR(context, "No pub key");

                tlv_free(decrypted_message);

                send_tlv_error_response(context, 6, TLVError_Authentication);
                break;
            }

            tlv_t *tlv_device_signature = tlv_get_value(decrypted_message, TLVType_Signature);
            if (!tlv_device_signature) {
                CLIENT_ERROR(context, "No sign");

                tlv_free(decrypted_message);

                send_tlv_error_response(context, 6, TLVError_Authentication);
                break;
            }

            CLIENT_DEBUG(context, "Importing device public key");
            ed25519_key *device_key = crypto_ed25519_new();
            r = crypto_ed25519_import_public_key(
                device_key,
                tlv_device_public_key->value, tlv_device_public_key->size
            );
            if (r) {
                CLIENT_ERROR(context, "Import pub key (%d)", r);

                crypto_ed25519_free(device_key);
                tlv_free(decrypted_message);

                send_tlv_error_response(context, 6, TLVError_Authentication);
                break;
            }

            byte device_x[HKDF_HASH_SIZE];
            size_t device_x_size = sizeof(device_x);

            CLIENT_DEBUG(context, "Calculating DeviceX");
            const char salt2[] = "Pair-Setup-Controller-Sign-Salt";
            const char info2[] = "Pair-Setup-Controller-Sign-Info";
            r = crypto_srp_hkdf(
                homekit_server->pairing_context->srp,
                (byte *)salt2, sizeof(salt2)-1,
                (byte *)info2, sizeof(info2)-1,
                device_x, &device_x_size
            );
            if (r) {
                CLIENT_ERROR(context, "Generate DevX (%d)", r);

                crypto_ed25519_free(device_key);
                tlv_free(decrypted_message);

                send_tlv_error_response(context, 6, TLVError_Authentication);
                break;
            }

            size_t device_info_size = device_x_size + tlv_device_id->size + tlv_device_public_key->size;
            byte *device_info = malloc(device_info_size);
            memcpy(device_info,
                   device_x,
                   device_x_size);
            memcpy(device_info + device_x_size,
                   tlv_device_id->value,
                   tlv_device_id->size);
            memcpy(device_info + device_x_size + tlv_device_id->size,
                   tlv_device_public_key->value,
                   tlv_device_public_key->size);
            
            CLIENT_DEBUG(context, "Verifying device signature");
            r = crypto_ed25519_verify(
                device_key,
                device_info, device_info_size,
                tlv_device_signature->value, tlv_device_signature->size
            );
            
            free(device_info);
            
            if (r) {
                CLIENT_ERROR(context, "Generate DevX (%d)", r);
                
                crypto_ed25519_free(device_key);
                tlv_free(decrypted_message);
                
                send_tlv_error_response(context, 6, TLVError_Authentication);
                break;
            }
            
            char *device_id = strndup((const char *)tlv_device_id->value, tlv_device_id->size);
            
            CLIENT_INFO(context, "Adding pairing %s, %i", device_id, pairing_permissions_admin);
            
            pairing_t *pairing = homekit_storage_find_pairing(device_id);
            if (pairing) {
                pairing_free(pairing);
                r = -10;
            } else {
                r = homekit_storage_add_pairing(device_id, device_key, pairing_permissions_admin);
            }
            
            crypto_ed25519_free(device_key);
            tlv_free(decrypted_message);
            
            free(device_id);
            
            if (r) {
                CLIENT_ERROR(context, "Store (%d)", r);
                
                send_tlv_error_response(context, 6, TLVError_Unknown);
                break;
            }
            
            HOMEKIT_NOTIFY_EVENT(homekit_server, HOMEKIT_EVENT_PAIRING_ADDED);

            CLIENT_DEBUG(context, "Exporting accessory public key");
            size_t accessory_public_key_size = 0;
            crypto_ed25519_export_public_key(homekit_server->accessory_key, NULL, &accessory_public_key_size);

            byte *accessory_public_key = malloc(accessory_public_key_size);
            r = crypto_ed25519_export_public_key(homekit_server->accessory_key, accessory_public_key, &accessory_public_key_size);
            if (r) {
                CLIENT_ERROR(context, "Export pub key (%d)", r);

                free(accessory_public_key);

                send_tlv_error_response(context, 6, TLVError_Authentication);
                break;
            }

            size_t accessory_id_size = strlen(homekit_server->accessory_id);
            size_t accessory_info_size = HKDF_HASH_SIZE + accessory_id_size + accessory_public_key_size;
            byte *accessory_info = malloc(accessory_info_size);

            CLIENT_DEBUG(context, "Calculating AccessoryX");
            size_t accessory_x_size = accessory_info_size;
            const char salt3[] = "Pair-Setup-Accessory-Sign-Salt";
            const char info3[] = "Pair-Setup-Accessory-Sign-Info";
            r = crypto_srp_hkdf(
                homekit_server->pairing_context->srp,
                (byte *)salt3, sizeof(salt3)-1,
                (byte *)info3, sizeof(info3)-1,
                accessory_info, &accessory_x_size
            );
            if (r) {
                CLIENT_ERROR(context, "Generate AccX (%d)", r);

                free(accessory_info);
                free(accessory_public_key);

                send_tlv_error_response(context, 6, TLVError_Unknown);
                break;
            }

            memcpy(accessory_info + accessory_x_size,
                   homekit_server->accessory_id, accessory_id_size);
            memcpy(accessory_info + accessory_x_size + accessory_id_size,
                   accessory_public_key, accessory_public_key_size);

            CLIENT_DEBUG(context, "Generating accessory sign");
            DEBUG_HEAP();
            size_t accessory_signature_size = 0;
            crypto_ed25519_sign(
                homekit_server->accessory_key,
                accessory_info, accessory_info_size,
                NULL, &accessory_signature_size
            );

            byte *accessory_signature = malloc(accessory_signature_size);
            r = crypto_ed25519_sign(
                homekit_server->accessory_key,
                accessory_info, accessory_info_size,
                accessory_signature, &accessory_signature_size
            );
            
            free(accessory_info);
            
            if (r) {
                CLIENT_ERROR(context, "Generate acc sign (%d)", r);
                
                free(accessory_signature);
                free(accessory_public_key);
                
                send_tlv_error_response(context, 6, TLVError_Unknown);
                break;
            }
            tlv_values_t *response_message = tlv_new();
            tlv_add_value(response_message, TLVType_Identifier,
                          (byte *)homekit_server->accessory_id, accessory_id_size);
            
            tlv_add_value(response_message, TLVType_PublicKey,
                          accessory_public_key, accessory_public_key_size);
            free(accessory_public_key);
            
            tlv_add_value(response_message, TLVType_Signature,
                          accessory_signature, accessory_signature_size);
            free(accessory_signature);

            size_t response_data_size = 0;
            TLV_DEBUG(response_message);

            tlv_format(response_message, NULL, &response_data_size);

            byte *response_data = malloc(response_data_size);
            r = tlv_format(response_message, response_data, &response_data_size);
            if (r) {
                CLIENT_ERROR(context, "Format TLV response (%d)", r);

                free(response_data);
                tlv_free(response_message);

                send_tlv_error_response(context, 6, TLVError_Unknown);
                break;
            }

            tlv_free(response_message);

            CLIENT_DEBUG(context, "Encrypting response");
            size_t encrypted_response_data_size = 0;
            crypto_chacha20poly1305_encrypt(
                shared_secret, (byte *)"\x0\x0\x0\x0PS-Msg06", NULL, 0,
                response_data, response_data_size,
                NULL, &encrypted_response_data_size
            );

            byte *encrypted_response_data = malloc(encrypted_response_data_size);
            r = crypto_chacha20poly1305_encrypt(
                shared_secret, (byte *)"\x0\x0\x0\x0PS-Msg06", NULL, 0,
                response_data, response_data_size,
                encrypted_response_data, &encrypted_response_data_size
            );

            free(response_data);

            if (r) {
                CLIENT_ERROR(context, "Encrypt response (%d)", r);

                free(encrypted_response_data);

                send_tlv_error_response(context, 6, TLVError_Unknown);
                break;
            }

            tlv_values_t *response = tlv_new();
            tlv_add_integer_value(response, TLVType_State, 1, 6);
            tlv_add_value(response, TLVType_EncryptedData,
                          encrypted_response_data, encrypted_response_data_size);
            free(encrypted_response_data);
            
            send_tlv_response(context, response);
            
            pairing_context_free(homekit_server->pairing_context);
            homekit_server->pairing_context = NULL;
            
            homekit_server->paired = true;
            
            homekit_mdns_buffer_set(0);
            homekit_setup_mdns();

            CLIENT_INFO(context, "Done 3/3");
            
            break;
        }
        
        default: {
            CLIENT_ERROR(context, "State %d", tlv_get_integer_value(message, TLVType_State, -1));
        }
    }
    
    homekit_server->is_pairing = false;
    
#ifdef HOMEKIT_OVERCLOCK_PAIR_SETUP
    sdk_system_restoreclock();
    HOMEKIT_DEBUG_LOG("CPU restoreclock");
#endif // HOMEKIT_OVERCLOCK_PAIR_SETUP
    
    tlv_free(message);
}

void homekit_server_on_pair_verify(client_context_t *context, const byte *data, size_t size) {
#ifdef HOMEKIT_PAIR_VERIFY_TIME_DEBUG
    uint32_t function_time = sdk_system_get_time_raw();
#endif
    
    HOMEKIT_DEBUG_LOG("Pair Verify");
    DEBUG_HEAP();
    
    tlv_values_t *message = tlv_new();
    if (tlv_parse(data, size, message)) {
        CLIENT_ERROR(context, "TLV");
        tlv_free(message);
        send_tlv_error_response(context, 2, TLVError_Unknown);
        return;
    }

    TLV_DEBUG(message);

#ifdef HOMEKIT_OVERCLOCK_PAIR_VERIFY
    sdk_system_overclock();
    HOMEKIT_DEBUG_LOG("CPU overclock");
#endif // HOMEKIT_OVERCLOCK_PAIR_VERIFY
    
    int r;

    switch(tlv_get_integer_value(message, TLVType_State, -1)) {
        case 1: {
            CLIENT_INFO(context, "Verify 1/2");

            CLIENT_DEBUG(context, "Importing device Curve public key");
            tlv_t *tlv_device_public_key = tlv_get_value(message, TLVType_PublicKey);
            if (!tlv_device_public_key) {
                CLIENT_ERROR(context, "Curve pub key not found");
                send_tlv_error_response(context, 2, TLVError_Unknown);
                break;
            }
            curve25519_key *device_key = crypto_curve25519_new();
            r = crypto_curve25519_import_public(
                device_key,
                tlv_device_public_key->value, tlv_device_public_key->size
            );
            if (r) {
                CLIENT_ERROR(context, "Import Curve pub key (%d)", r);
                crypto_curve25519_free(device_key);
                send_tlv_error_response(context, 2, TLVError_Unknown);
                break;
            }

            CLIENT_DEBUG(context, "Generating accessory Curve25519 key");
            curve25519_key *my_key = crypto_curve25519_generate();
            if (!my_key) {
                CLIENT_ERROR(context, "Generate acc Curve key");
                crypto_curve25519_free(device_key);
                send_tlv_error_response(context, 2, TLVError_Unknown);
                break;
            }

            CLIENT_DEBUG(context, "Exporting accessory Curve25519 public key");
            size_t my_key_public_size = 0;
            crypto_curve25519_export_public(my_key, NULL, &my_key_public_size);

            byte *my_key_public = malloc(my_key_public_size);
            r = crypto_curve25519_export_public(my_key, my_key_public, &my_key_public_size);
            if (r) {
                CLIENT_ERROR(context, "Export Curve pub key (%d)", r);
                free(my_key_public);
                crypto_curve25519_free(my_key);
                crypto_curve25519_free(device_key);
                send_tlv_error_response(context, 2, TLVError_Unknown);
                break;
            }
            
            CLIENT_DEBUG(context, "Generating Curve25519 shared secret");
            size_t shared_secret_size = 0;
            crypto_curve25519_shared_secret(my_key, device_key, NULL, &shared_secret_size);

            byte *shared_secret = malloc(shared_secret_size);
            r = crypto_curve25519_shared_secret(my_key, device_key, shared_secret, &shared_secret_size);
            crypto_curve25519_free(my_key);
            crypto_curve25519_free(device_key);

            if (r) {
                CLIENT_ERROR(context, "Generate Curve shared secret (%d)", r);
                free(shared_secret);
                free(my_key_public);
                send_tlv_error_response(context, 2, TLVError_Unknown);
                break;
            }

            CLIENT_DEBUG(context, "Generating sign");
            size_t accessory_id_size = strlen(homekit_server->accessory_id);
            size_t accessory_info_size = my_key_public_size + accessory_id_size + tlv_device_public_key->size;

            byte *accessory_info = malloc(accessory_info_size);
            memcpy(accessory_info,
                   my_key_public, my_key_public_size);
            memcpy(accessory_info + my_key_public_size,
                   homekit_server->accessory_id, accessory_id_size);
            memcpy(accessory_info + my_key_public_size + accessory_id_size,
                   tlv_device_public_key->value, tlv_device_public_key->size);

            size_t accessory_signature_size = 0;
            crypto_ed25519_sign(
                homekit_server->accessory_key,
                accessory_info, accessory_info_size,
                NULL, &accessory_signature_size
            );

            byte *accessory_signature = malloc(accessory_signature_size);
            r = crypto_ed25519_sign(
                homekit_server->accessory_key,
                accessory_info, accessory_info_size,
                accessory_signature, &accessory_signature_size
            );
            free(accessory_info);
            if (r) {
                CLIENT_ERROR(context, "Generate sign (%d)", r);
                free(accessory_signature);
                free(shared_secret);
                free(my_key_public);
                send_tlv_error_response(context, 2, TLVError_Unknown);
                break;
            }

            tlv_values_t *sub_response = tlv_new();
            tlv_add_value(sub_response, TLVType_Identifier,
                          (const byte *)homekit_server->accessory_id, accessory_id_size);
            tlv_add_value(sub_response, TLVType_Signature,
                          accessory_signature, accessory_signature_size);
            free(accessory_signature);

            size_t sub_response_data_size = 0;
            tlv_format(sub_response, NULL, &sub_response_data_size);

            byte *sub_response_data = malloc(sub_response_data_size);
            r = tlv_format(sub_response, sub_response_data, &sub_response_data_size);
            tlv_free(sub_response);

            if (r) {
                CLIENT_ERROR(context, "Format sub-TLV message (%d)", r);
                free(sub_response_data);
                free(shared_secret);
                free(my_key_public);
                send_tlv_error_response(context, 2, TLVError_Unknown);
                break;
            }

            CLIENT_DEBUG(context, "Generating proof");
            size_t session_key_size = 0;
            const byte salt[] = "Pair-Verify-Encrypt-Salt";
            const byte info[] = "Pair-Verify-Encrypt-Info";
            crypto_hkdf(
                shared_secret, shared_secret_size,
                salt, sizeof(salt)-1,
                info, sizeof(info)-1,
                NULL, &session_key_size
            );

            byte *session_key = malloc(session_key_size);
            r = crypto_hkdf(
                shared_secret, shared_secret_size,
                salt, sizeof(salt)-1,
                info, sizeof(info)-1,
                session_key, &session_key_size
            );
            if (r) {
                CLIENT_ERROR(context, "Derive session key (%d)", r);
                free(session_key);
                free(sub_response_data);
                free(shared_secret);
                free(my_key_public);
                send_tlv_error_response(context, 2, TLVError_Unknown);
                break;
            }

            CLIENT_DEBUG(context, "Encrypting response");
            size_t encrypted_response_data_size = 0;
            crypto_chacha20poly1305_encrypt(
                session_key, (byte *)"\x0\x0\x0\x0PV-Msg02", NULL, 0,
                sub_response_data, sub_response_data_size,
                NULL, &encrypted_response_data_size
            );

            byte *encrypted_response_data = malloc(encrypted_response_data_size);
            r = crypto_chacha20poly1305_encrypt(
                session_key, (byte *)"\x0\x0\x0\x0PV-Msg02", NULL, 0,
                sub_response_data, sub_response_data_size,
                encrypted_response_data, &encrypted_response_data_size
            );
            free(sub_response_data);

            if (r) {
                CLIENT_ERROR(context, "Encrypt sub response data (%d)", r);
                free(encrypted_response_data);
                free(session_key);
                free(shared_secret);
                free(my_key_public);
                send_tlv_error_response(context, 2, TLVError_Unknown);
                break;
            }

            tlv_values_t *response = tlv_new();
            tlv_add_integer_value(response, TLVType_State, 1, 2);
            tlv_add_value(response, TLVType_PublicKey,
                          my_key_public, my_key_public_size);
            tlv_add_value(response, TLVType_EncryptedData,
                          encrypted_response_data, encrypted_response_data_size);
            free(encrypted_response_data);
            
            send_tlv_response(context, response);
            
            if (context->verify_context) {
                pair_verify_context_free(&context->verify_context);
            }

            context->verify_context = pair_verify_context_new();
            context->verify_context->secret = shared_secret;
            context->verify_context->secret_size = shared_secret_size;

            context->verify_context->session_key = session_key;
            context->verify_context->session_key_size = session_key_size;

            context->verify_context->accessory_public_key = my_key_public;
            context->verify_context->accessory_public_key_size = my_key_public_size;

            context->verify_context->device_public_key = malloc(tlv_device_public_key->size);
            memcpy(context->verify_context->device_public_key,
                   tlv_device_public_key->value, tlv_device_public_key->size);
            context->verify_context->device_public_key_size = tlv_device_public_key->size;

            break;
        }
        case 3: {
            CLIENT_INFO(context, "Verify 2/2");
            
            if (!context->verify_context) {
                CLIENT_ERROR(context, "No state 1 data");
                send_tlv_error_response(context, 4, TLVError_Authentication);
                break;
            }

            tlv_t *tlv_encrypted_data = tlv_get_value(message, TLVType_EncryptedData);
            if (!tlv_encrypted_data) {
                CLIENT_ERROR(context, "No encrypted data");

                pair_verify_context_free(&context->verify_context);

                send_tlv_error_response(context, 4, TLVError_Authentication);
                break;
            }

            CLIENT_DEBUG(context, "Decrypt payload");
            size_t decrypted_data_size = 0;
            crypto_chacha20poly1305_decrypt(
                context->verify_context->session_key, (byte *)"\x0\x0\x0\x0PV-Msg03", NULL, 0,
                tlv_encrypted_data->value, tlv_encrypted_data->size,
                NULL, &decrypted_data_size
            );

            byte *decrypted_data = malloc(decrypted_data_size);
            r = crypto_chacha20poly1305_decrypt(
                context->verify_context->session_key, (byte *)"\x0\x0\x0\x0PV-Msg03", NULL, 0,
                tlv_encrypted_data->value, tlv_encrypted_data->size,
                decrypted_data, &decrypted_data_size
            );
            if (r) {
                CLIENT_ERROR(context, "Decrypt data (%d)", r);

                free(decrypted_data);
                pair_verify_context_free(&context->verify_context);

                send_tlv_error_response(context, 4, TLVError_Authentication);
                break;
            }

            tlv_values_t *decrypted_message = tlv_new();
            r = tlv_parse(decrypted_data, decrypted_data_size, decrypted_message);
            free(decrypted_data);

            if (r) {
                CLIENT_ERROR(context, "Parse TLV (%d)", r);

                tlv_free(decrypted_message);
                pair_verify_context_free(&context->verify_context);

                send_tlv_error_response(context, 4, TLVError_Authentication);
                break;
            }

            tlv_t *tlv_device_id = tlv_get_value(decrypted_message, TLVType_Identifier);
            if (!tlv_device_id) {
                CLIENT_ERROR(context, "No id");

                tlv_free(decrypted_message);
                pair_verify_context_free(&context->verify_context);

                send_tlv_error_response(context, 4, TLVError_Authentication);
                break;
            }
            
            tlv_t *tlv_device_signature = tlv_get_value(decrypted_message, TLVType_Signature);
            if (!tlv_device_signature) {
                CLIENT_ERROR(context, "No sign");

                tlv_free(decrypted_message);
                pair_verify_context_free(&context->verify_context);

                send_tlv_error_response(context, 4, TLVError_Authentication);
                break;
            }

            char *device_id = strndup((const char *)tlv_device_id->value, tlv_device_id->size);
            CLIENT_DEBUG(context, "Searching pairing for %s", device_id);
            pairing_t *pairing = homekit_storage_find_pairing(device_id);
            if (!pairing) {
                CLIENT_ERROR(context, "No pairing %s", device_id);

                free(device_id);
                tlv_free(decrypted_message);
                pair_verify_context_free(&context->verify_context);

                send_tlv_error_response(context, 4, TLVError_Authentication);
                break;
            }
            
            CLIENT_INFO(context, "Verify %s", device_id);
            
            free(device_id);
            
            byte permissions = pairing->permissions;
            int pairing_id = pairing->id;
            
            size_t device_info_size =
                context->verify_context->device_public_key_size +
                context->verify_context->accessory_public_key_size +
                tlv_device_id->size;

            byte *device_info = malloc(device_info_size);
            memcpy(device_info,
                   context->verify_context->device_public_key, context->verify_context->device_public_key_size);
            memcpy(device_info + context->verify_context->device_public_key_size,
                   tlv_device_id->value, tlv_device_id->size);
            memcpy(device_info + context->verify_context->device_public_key_size + tlv_device_id->size,
                   context->verify_context->accessory_public_key, context->verify_context->accessory_public_key_size);

            CLIENT_DEBUG(context, "Verifying sign");
            r = crypto_ed25519_verify(
                pairing->device_key,
                device_info, device_info_size,
                tlv_device_signature->value, tlv_device_signature->size
            );
            free(device_info);
            pairing_free(pairing);
            tlv_free(decrypted_message);

            if (r) {
                CLIENT_ERROR(context, "Verify sign (%d)", r);

                pair_verify_context_free(&context->verify_context);

                send_tlv_error_response(context, 4, TLVError_Authentication);
                break;
            }

            const byte salt[] = "Control-Salt";

            size_t read_key_size = 32;
            const byte read_info[] = "Control-Read-Encryption-Key";
            r = crypto_hkdf(
                context->verify_context->secret, context->verify_context->secret_size,
                salt, sizeof(salt)-1,
                read_info, sizeof(read_info)-1,
                context->read_key, &read_key_size
            );

            if (r) {
                CLIENT_ERROR(context, "Derive read enc key (%d)", r);

                pair_verify_context_free(&context->verify_context);

                send_tlv_error_response(context, 4, TLVError_Unknown);
                break;
            }

            size_t write_key_size = 32;
            const byte write_info[] = "Control-Write-Encryption-Key";
            r = crypto_hkdf(
                context->verify_context->secret, context->verify_context->secret_size,
                salt, sizeof(salt)-1,
                write_info, sizeof(write_info)-1,
                context->write_key, &write_key_size
            );

            pair_verify_context_free(&context->verify_context);
            
            if (r) {
                CLIENT_ERROR(context, "Derive write enc key (%d)", r);

                send_tlv_error_response(context, 4, TLVError_Unknown);
                break;
            }

            tlv_values_t *response = tlv_new();
            tlv_add_integer_value(response, TLVType_State, 1, 4);
            
            send_tlv_response(context, response);

            context->pairing_id = pairing_id;
            context->permissions = permissions;
            context->encrypted = true;

            HOMEKIT_NOTIFY_EVENT(homekit_server, HOMEKIT_EVENT_CLIENT_VERIFIED);

            CLIENT_INFO(context, "Verify OK");

            break;
        }
        default: {
            CLIENT_ERROR(context, "Unknown state %d", tlv_get_integer_value(message, TLVType_State, -1));
        }
    }
    
#ifdef HOMEKIT_OVERCLOCK_PAIR_VERIFY
    sdk_system_restoreclock();
    HOMEKIT_DEBUG_LOG("CPU restoreclock");
#endif // HOMEKIT_OVERCLOCK_PAIR_VERIFY

    tlv_free(message);
    
#ifdef HOMEKIT_PAIR_VERIFY_TIME_DEBUG
    CLIENT_INFO(context, "Verify Time %d", sdk_system_get_time_raw() - function_time);
#endif
}


void homekit_server_on_get_accessories(client_context_t *context) {
    CLIENT_INFO(context, "Get ACC");
    DEBUG_HEAP();
    
    json_stream* json = &homekit_server->json;
    json_init(json, context);
    
    if (send_200_response(context) < 0) {
        json->error = true;
    }
    
    json_object_start(json);
    json_string(json, "accessories"); json_array_start(json);

    for (homekit_accessory_t **accessory_it = homekit_server->config->accessories; *accessory_it; accessory_it++) {
        homekit_accessory_t *accessory = *accessory_it;

        json_object_start(json);

        json_string(json, "aid"); json_integer(json, accessory->id);
        json_string(json, "services"); json_array_start(json);

        for (homekit_service_t **service_it = accessory->services; *service_it; service_it++) {
            homekit_service_t *service = *service_it;

            json_object_start(json);

            json_string(json, "iid"); json_integer(json, service->id);
            json_string(json, "type"); json_string(json, service->type);
            json_string(json, "primary"); json_boolean(json, service->primary);
            json_string(json, "hidden"); json_boolean(json, service->hidden);
            if (service->linked) {
                json_string(json, "linked"); json_array_start(json);
                for (homekit_service_t **linked=service->linked; *linked; linked++) {
                    json_integer(json, (*linked)->id);
                    
                    if (json->error) {
                        break;
                    }
                }
                json_array_end(json);
            }

            json_string(json, "characteristics"); json_array_start(json);

            for (homekit_characteristic_t **ch_it = service->characteristics; *ch_it; ch_it++) {
                homekit_characteristic_t *ch = *ch_it;

                json_object_start(json);
                write_characteristic_json(
                    json, context, ch,
                      characteristic_format_type
                    | characteristic_format_meta
                    | characteristic_format_perms
                    | characteristic_format_events,
                    NULL,
                    accessory->id
                );
                json_object_end(json);
                
                if (json->error) {
                    break;
                }
            }

            json_array_end(json);
            json_object_end(json); // service
            
            if (json->error) {
                break;
            }
        }

        json_array_end(json);
        json_object_end(json); // accessory
        
        if (json->error) {
            break;
        }
    }
    
    json_array_end(json);
    json_object_end(json); // response
    
    json_flush(json);
    //json_buffer_free(json);
    //free(json);
    
    if (json->error) {
        CLIENT_ERROR(context, "JSON");
    }
    
    client_send_chunk(NULL, 0, context);
}

void homekit_server_on_get_characteristics(client_context_t *context) {
    CLIENT_INFO(context, "Get CH");
    DEBUG_HEAP();
    
    //unsigned int time_start = sdk_system_get_time_raw();
    
    query_param_t *qp = context->endpoint_params;
    while (qp) {
        CLIENT_DEBUG(context, "Query paramter %s = %s", qp->name, qp->value);
        qp = qp->next;
    }

    query_param_t *id_param = query_params_find(context->endpoint_params, "id");
    if (!id_param) {
        CLIENT_ERROR(context, "No ID param");
        send_json_error_response(context, 400, HAPStatus_InvalidValue);
        return;
    }

    int bool_endpoint_param(const char *name) {
        query_param_t *param = query_params_find(context->endpoint_params, name);
        return param && param->value && !strcmp(param->value, "1");
    }

    characteristic_format_t format = 0;
    if (bool_endpoint_param("meta"))
        format |= characteristic_format_meta;

    if (bool_endpoint_param("perms"))
        format |= characteristic_format_perms;

    if (bool_endpoint_param("type"))
        format |= characteristic_format_type;

    if (bool_endpoint_param("ev"))
        format |= characteristic_format_events;

    unsigned int success = true;

    char *id = strdup(id_param->value);
    char *ch_id;
    char *_id = id;
    while ((ch_id = strsep(&_id, ","))) {
        char *dot = strstr(ch_id, ".");
        if (!dot) {
            send_json_error_response(context, 400, HAPStatus_InvalidValue);
            free(id);
            return;
        }

        *dot = 0;
        int aid = atoi(ch_id);
        int iid = atoi(dot+1);

        CLIENT_DEBUG(context, "Requested characteristic info for %d.%d", aid, iid);
        homekit_characteristic_t *ch = homekit_characteristic_by_aid_and_iid(homekit_server->config->accessories, aid, iid);
        if (!ch) {
            success = false;
            continue;
        }

        if (!(ch->permissions & HOMEKIT_PERMISSIONS_PAIRED_READ)) {
            success = false;
            continue;
        }
    }

    free(id);
    id = strdup(id_param->value);

    json_stream* json = &homekit_server->json;
    json_init(json, context);
    
    int resul = -1;
    
    if (success) {
        resul = send_200_response(context);
    } else {
        resul = send_207_response(context);
    }
    
    if (resul < 0) {
        json->error = true;
    }
    
    json_object_start(json);
    json_string(json, "characteristics"); json_array_start(json);

    void write_characteristic_error(json_stream *json, int aid, int iid, int status) {
        json_object_start(json);
        json_string(json, "aid"); json_integer(json, aid);
        json_string(json, "iid"); json_integer(json, iid);
        json_string(json, "status"); json_integer(json, status);
        json_object_end(json);
    }

    _id = id;
    while ((ch_id = strsep(&_id, ","))) {
        char *dot = strstr(ch_id, ".");
        *dot = 0;
        int aid = atoi(ch_id);
        int iid = atoi(dot+1);
        
        CLIENT_DEBUG(context, "for %d.%d", aid, iid);
        homekit_characteristic_t *ch = homekit_characteristic_by_aid_and_iid(homekit_server->config->accessories, aid, iid);
        if (!ch) {
            write_characteristic_error(json, aid, iid, HAPStatus_NoResource);
            continue;
        }

        if (!(ch->permissions & HOMEKIT_PERMISSIONS_PAIRED_READ)) {
            write_characteristic_error(json, aid, iid, HAPStatus_WriteOnly);
            continue;
        }

        json_object_start(json);
        write_characteristic_json(json, context, ch, format, NULL, aid);
        if (!success) {
            json_string(json, "status"); json_integer(json, HAPStatus_Success);
        }
        json_object_end(json);
        
        if (json->error) {
            break;
        }
    }

    json_array_end(json);
    json_object_end(json); // response

    json_flush(json);
    //json_buffer_free(json);
    //free(json);
    
    free(id);

    if (json->error) {
        CLIENT_ERROR(context, "JSON");
    }
    
    client_send_chunk(NULL, 0, context);
    
    //CLIENT_INFO(context, "Time %i", sdk_system_get_time_raw() - time_start);
}

void homekit_server_on_update_characteristics(client_context_t *context, const byte *data, size_t size) {
    CLIENT_INFO(context, "Upd CH");
    DEBUG_HEAP();
    
    cJSON_rsf *json = cJSON_rsf_Parse((char*) data);

    if (!json) {
        CLIENT_ERROR(context, "Parse JSON");
        send_json_error_response(context, 400, HAPStatus_InvalidValue);
        return;
    }

    cJSON_rsf *characteristics = cJSON_rsf_GetObjectItem(json, "characteristics");
    if (!characteristics) {
        CLIENT_ERROR(context, "No \"characteristics\"");
        cJSON_rsf_Delete(json);
        send_json_error_response(context, 400, HAPStatus_InvalidValue);
        return;
    }
    
    if (characteristics->type != cJSON_rsf_Array) {
        CLIENT_ERROR(context, "\"characteristics\" no list");
        cJSON_rsf_Delete(json);
        send_json_error_response(context, 400, HAPStatus_InvalidValue);
        return;
    }

    HAPStatus process_characteristics_update(const cJSON_rsf *j_ch) {
        cJSON_rsf *j_aid = cJSON_rsf_GetObjectItem(j_ch, "aid");
        if (!j_aid) {
            CLIENT_ERROR(context, "No \"aid\"");
            return HAPStatus_NoResource;
        }
        if (j_aid->type != cJSON_rsf_Number) {
            CLIENT_ERROR(context, "\"aid\" no number");
            return HAPStatus_NoResource;
        }
        
        cJSON_rsf *j_iid = cJSON_rsf_GetObjectItem(j_ch, "iid");
        if (!j_iid) {
            CLIENT_ERROR(context, "No \"iid\"");
            return HAPStatus_NoResource;
        }
        if (j_iid->type != cJSON_rsf_Number) {
            CLIENT_ERROR(context, "\"iid\" no number");
            return HAPStatus_NoResource;
        }
        
        int aid = j_aid->valuefloat;
        int iid = j_iid->valuefloat;
        
        homekit_characteristic_t *ch = homekit_characteristic_by_aid_and_iid(
            homekit_server->config->accessories, aid, iid
        );
        if (!ch) {
            CLIENT_ERROR(context, "for %d.%d: no ch", aid, iid);
            return HAPStatus_NoResource;
        }
        
        cJSON_rsf *j_value = cJSON_rsf_GetObjectItem(j_ch, "value");
        if (j_value) {
            homekit_value_t h_value = HOMEKIT_NULL();

            if (!(ch->permissions & HOMEKIT_PERMISSIONS_PAIRED_WRITE)) {
                CLIENT_ERROR(context, "for %d.%d: no PW", aid, iid);
                return HAPStatus_ReadOnly;
            }

            switch (ch->format) {
                case HOMEKIT_FORMAT_BOOL: {
                    unsigned int value = false;
                    if (j_value->type == cJSON_rsf_True) {
                        value = true;
                    } else if (j_value->type == cJSON_rsf_False) {
                        value = false;
                    } else if (j_value->type == cJSON_rsf_Number &&
                            (j_value->valuefloat == 0 || j_value->valuefloat == 1)) {
                        value = j_value->valuefloat == 1;
                    } else {
                        CLIENT_ERROR(context, "for %d.%d: no bool or 0/1", aid, iid);
                        return HAPStatus_InvalidValue;
                    }

                    CLIENT_DEBUG(context, "for %d.%d=%i", aid, iid, value);
                    
                    h_value = HOMEKIT_BOOL(value);
                    if (ch->setter_ex) {
                        ch->setter_ex(ch, h_value);
                    } else {
                        ch->value = h_value;
                    }
                    break;
                }
                case HOMEKIT_FORMAT_UINT8:
                case HOMEKIT_FORMAT_UINT16:
                case HOMEKIT_FORMAT_UINT32:
                case HOMEKIT_FORMAT_UINT64:
                case HOMEKIT_FORMAT_INT: {
                    // We accept boolean values here in order to fix a bug in HomeKit. HomeKit sometimes sends a boolean instead of an integer of value 0 or 1.
                    if (j_value->type != cJSON_rsf_Number && j_value->type != cJSON_rsf_False && j_value->type != cJSON_rsf_True) {
                        CLIENT_ERROR(context, "for %d.%d: no number", aid, iid);
                        return HAPStatus_InvalidValue;
                    }

                    int min_value = 0;
                    //unsigned long long max_value = 0;
                    long long max_value = 0;

                    switch (ch->format) {
                        case HOMEKIT_FORMAT_UINT8:
                            min_value = 0;
                            max_value = 255;
                            break;
                        
                        case HOMEKIT_FORMAT_UINT16:
                            min_value = 0;
                            max_value = 65535;
                            break;
                        
                        /*
                        case HOMEKIT_FORMAT_UINT32:
                            min_value = 0;
                            max_value = 4294967295;
                            break;
                        
                        case HOMEKIT_FORMAT_UINT64:
                            min_value = 0;
                            max_value = 18446744073709551615ULL;
                            break;
                        */
                        
                        case HOMEKIT_FORMAT_UINT32:
                        case HOMEKIT_FORMAT_UINT64:
                            min_value = 0;
                            max_value = 2147483647;
                            break;
                        
                        case HOMEKIT_FORMAT_INT:
                            min_value = -2147483648;
                            max_value = 2147483647;
                            break;
                    
                        default:
                            // Impossible, keeping to make compiler happy
                            break;
                    }

                    // Old style
                    if (ch->min_value) {
                        min_value = (int) *ch->min_value;
                    }
                    if (ch->max_value) {
                        max_value = (int) *ch->max_value;
                    }

                    int value = j_value->valuefloat;

                    // New style
                    /*
                    if (ch->min_value) {
                        min_value = *ch->min_value;
                    }
                    if (ch->max_value) {
                        max_value = *ch->max_value;
                    }
                    
                    double value = j_value->valuefloat;
                    */
                    
                    if (j_value->type == cJSON_rsf_True) {
                        value = 1;
                    } else if (j_value->type == cJSON_rsf_False) {
                        value = 0;
                    }
                    
                    
                    /*
                    if (value < min_value || value > max_value) {
                        CLIENT_ERROR(context, "Update %d.%d: not in range", aid, iid);
                        return HAPStatus_InvalidValue;
                    }
                    */
                    if (value < min_value) {
                        value = min_value;
                    } else if (value > max_value) {
                        value = max_value;
                    }

                    
                    if (ch->valid_values.count) {
                        unsigned int matches = false;
                        for (unsigned int i = 0; i < ch->valid_values.count; i++) {
                            if (value == ch->valid_values.values[i]) {
                                matches = true;
                                break;
                            }
                        }

                        if (!matches) {
                            CLIENT_ERROR(context, "for %d.%d: invalid values", aid, iid);
                            return HAPStatus_InvalidValue;
                        }
                    }
                    
#ifndef HOMEKIT_DISABLE_VALUE_RANGES
                    if (ch->valid_values_ranges.count) {
                        unsigned int matches = false;
                        for (unsigned int i = 0; i < ch->valid_values_ranges.count; i++) {
                            if (value >= ch->valid_values_ranges.ranges[i].start &&
                                    value <= ch->valid_values_ranges.ranges[i].end) {
                                matches = true;
                                break;
                            }
                        }

                        if (!matches) {
                            CLIENT_ERROR(context, "for %d.%d: range", aid, iid);
                            return HAPStatus_InvalidValue;
                        }
                    }
#endif //HOMEKIT_DISABLE_VALUE_RANGES
                    
                    CLIENT_DEBUG(context, "for %d.%d=%d", aid, iid, value);

                    // Old style
                    h_value = HOMEKIT_INT(value);
                    h_value.format = ch->format;
                    
                    /*
                    // New style
                    switch (ch->format) {
                        case HOMEKIT_FORMAT_UINT8:
                            h_value = HOMEKIT_UINT8(value);
                            break;
                        case HOMEKIT_FORMAT_UINT16:
                            h_value = HOMEKIT_UINT16(value);
                            break;
                        case HOMEKIT_FORMAT_UINT32:
                            h_value = HOMEKIT_UINT32(value);
                            break;
                        case HOMEKIT_FORMAT_UINT64:
                            h_value = HOMEKIT_UINT64(value);
                            break;
                        case HOMEKIT_FORMAT_INT:
                            h_value = HOMEKIT_INT(value);
                            break;

                        default:
                            CLIENT_ERROR(context, "Unexpected format when updating numeric value: %d", ch->format);
                            return HAPStatus_InvalidValue;
                    }
                    */
                    
                    if (ch->setter_ex) {
                        ch->setter_ex(ch, h_value);
                    } else {
                        ch->value = h_value;
                    }
                    break;
                }
                case HOMEKIT_FORMAT_FLOAT: {
                    if (j_value->type != cJSON_rsf_Number) {
                        CLIENT_ERROR(context, "for %d.%d: no number", aid, iid);
                        return HAPStatus_InvalidValue;
                    }

                    float value = j_value->valuefloat;
                    if ((ch->min_value && value < *ch->min_value) ||
                            (ch->max_value && value > *ch->max_value)) {
                        CLIENT_ERROR(context, "for %d.%d: out range", aid, iid);
                        return HAPStatus_InvalidValue;
                    }

                    CLIENT_DEBUG(context, "for %d.%d=%g", aid, iid, value);

                    h_value = HOMEKIT_FLOAT(value);
                    if (ch->setter_ex) {
                        ch->setter_ex(ch, h_value);
                    } else {
                        ch->value = h_value;
                    }
                    break;
                }
                case HOMEKIT_FORMAT_STRING: {
                    if (j_value->type != cJSON_rsf_String) {
                        CLIENT_ERROR(context, "for %d.%d: no string", aid, iid);
                        return HAPStatus_InvalidValue;
                    }

#ifndef HOMEKIT_DISABLE_MAXLEN_CHECK
                    unsigned int max_len = (ch->max_len) ? *ch->max_len : 64;
#endif //HOMEKIT_DISABLE_MAXLEN_CHECK
                    
                    char *value = j_value->valuestring;
                    
#ifndef HOMEKIT_DISABLE_MAXLEN_CHECK
                    if (strlen(value) > max_len) {
                        CLIENT_ERROR(context, "for %d.%d: too long", aid, iid);
                        return HAPStatus_InvalidValue;
                    }
#endif //HOMEKIT_DISABLE_MAXLEN_CHECK

                    CLIENT_DEBUG(context, "for %d.%d=\"%s\"", aid, iid, value);
                    
                    h_value = HOMEKIT_STRING(value);
                    if (ch->setter_ex) {
                        ch->setter_ex(ch, h_value);
                    } else {
                        homekit_value_destruct(&ch->value);
                        homekit_value_copy(&ch->value, &h_value);
                    }
                    break;
                }
                case HOMEKIT_FORMAT_TLV: {
                    if (j_value->type != cJSON_rsf_String) {
                        CLIENT_ERROR(context, "for %d.%d: no string", aid, iid);
                        return HAPStatus_InvalidValue;
                    }

#ifndef HOMEKIT_DISABLE_MAXLEN_CHECK
                    unsigned int max_len = (ch->max_len) ? *ch->max_len : 256;
#endif //HOMEKIT_DISABLE_MAXLEN_CHECK
                    
                    char *value = j_value->valuestring;
                    unsigned int value_len = strlen(value);
                    
#ifndef HOMEKIT_DISABLE_MAXLEN_CHECK
                    if (value_len > max_len) {
                        CLIENT_ERROR(context, "for %d.%d: too long", aid, iid);
                        return HAPStatus_InvalidValue;
                    }
#endif //HOMEKIT_DISABLE_MAXLEN_CHECK

                    size_t tlv_size = base64_decoded_size((unsigned char*)value, value_len);
                    byte *tlv_data = malloc(tlv_size);
                    if (base64_decode((byte*) value, value_len, tlv_data) < 0) {
                        free(tlv_data);
                        CLIENT_ERROR(context, "for %d.%d: Base64", aid, iid);
                        return HAPStatus_InvalidValue;
                    }

                    tlv_values_t *tlv_values = tlv_new();
                    int r = tlv_parse(tlv_data, tlv_size, tlv_values);
                    free(tlv_data);
                    
                    if (r) {
                        CLIENT_ERROR(context, "for %d.%d: parsing TLV", aid, iid);
                        return HAPStatus_InvalidValue;
                    }

                    CLIENT_DEBUG(context, "for %d.%d with TLV:", aid, iid);
                    for (tlv_t *t=tlv_values->head; t; t=t->next) {
                        char *escaped_payload = binary_to_string(t->value, t->size);
                        CLIENT_DEBUG(context, " Type %d value (%d bytes): %s", t->type, t->size, escaped_payload);
                        free(escaped_payload);
                    }
                    
                    h_value = HOMEKIT_TLV(tlv_values);
                    if (ch->setter_ex) {
                        ch->setter_ex(ch, h_value);
                    } else {
                        homekit_value_destruct(&ch->value);
                        homekit_value_copy(&ch->value, &h_value);
                    }
                    
                    tlv_free(tlv_values);
                    break;
                }
                case HOMEKIT_FORMAT_DATA: {
                    if (j_value->type != cJSON_rsf_String) {
                        CLIENT_ERROR(context, "for %d.%d: no string", aid, iid);
                        return HAPStatus_InvalidValue;
                    }
                    
                    // Default max data len = 2,097,152 but that does not make sense
                    // for this accessory
#ifndef HOMEKIT_DISABLE_MAXLEN_CHECK
                    unsigned int max_len = (ch->max_data_len) ? *ch->max_data_len : 16384;
#endif //HOMEKIT_DISABLE_MAXLEN_CHECK
                    
                    char *value = j_value->valuestring;
                    unsigned int value_len = strlen(value);
                    
#ifndef HOMEKIT_DISABLE_MAXLEN_CHECK
                    if (value_len > max_len) {
                        CLIENT_ERROR(context, "for %d.%d: too long", aid, iid);
                        return HAPStatus_InvalidValue;
                    }
#endif //HOMEKIT_DISABLE_MAXLEN_CHECK
                    
                    size_t data_size = base64_decoded_size((unsigned char*) value, value_len);
                    byte *data = malloc(data_size);
                    if (!data) {
                        CLIENT_ERROR(context, "for %d.%d: allocating %d", aid, iid, data_size);
                        return HAPStatus_InvalidValue;
                    }

                    if (base64_decode((byte*) value, value_len, data) < 0) {
                        free(data);
                        CLIENT_ERROR(context, "for %d.%d: Base64 decoding", aid, iid);
                        return HAPStatus_InvalidValue;
                    }
                    
                    CLIENT_DEBUG(context, "for %d.%d", aid, iid);

                    h_value = HOMEKIT_DATA(data, data_size);
                    if (ch->setter_ex) {
                        ch->setter_ex(ch, h_value);
                    } else {
                        homekit_value_destruct(&ch->value);
                        homekit_value_copy(&ch->value, &h_value);
                    }
                    
                    free(data);
                    break;
                }
                default: {
                    CLIENT_ERROR(context, "Update %d.%d: format %d", aid, iid, ch->format);
                    return HAPStatus_InvalidValue;
                }
            }
        }

        cJSON_rsf *j_events = cJSON_rsf_GetObjectItem(j_ch, "ev");
        if (j_events) {
            if (!(ch->permissions & HOMEKIT_PERMISSIONS_NOTIFY)) {
                CLIENT_ERROR(context, "for %d.%d: notif no supported", aid, iid);
                return HAPStatus_NotificationsUnsupported;
            }
            
            if ((j_events->type != cJSON_rsf_True) && (j_events->type != cJSON_rsf_False)) {
                CLIENT_ERROR(context, "for %d.%d: notif invalid state", aid, iid);
            }

            if (j_events->type == cJSON_rsf_True) {
                homekit_characteristic_add_notify_subscription(ch, context);
            } else {
                homekit_characteristic_remove_notify_subscription(ch, context);
            }
        }

        return HAPStatus_Success;
    }

    HAPStatus *statuses = malloc(sizeof(HAPStatus) * cJSON_rsf_GetArraySize(characteristics));
    unsigned int has_errors = false;
    for (unsigned int i = 0; i < cJSON_rsf_GetArraySize(characteristics); i++) {
        cJSON_rsf *j_ch = cJSON_rsf_GetArrayItem(characteristics, i);

#ifdef HOMEKIT_DEBUG
        char *s = cJSON_rsf_Print(j_ch);
        CLIENT_INFO(context, "Processing Ch: %s", s);
        free(s);
#endif
        
        statuses[i] = process_characteristics_update(j_ch);

        if (statuses[i] != HAPStatus_Success)
            has_errors = true;
    }

    if (!has_errors) {
        CLIENT_DEBUG(context, "There were no processing errors, sending No Content response");
        
        send_204_response(context);
    } else {
        CLIENT_DEBUG(context, "There were processing errors, sending Multi-Status response");
        
        json_stream* json1 = &homekit_server->json;
        json_init(json1, context);
        
        if (send_207_response(context) < 0) {
            json1->error = true;
        }
        
        json_object_start(json1);
        json_string(json1, "characteristics"); json_array_start(json1);

        for (unsigned int i = 0; i < cJSON_rsf_GetArraySize(characteristics); i++) {
            cJSON_rsf *j_ch = cJSON_rsf_GetArrayItem(characteristics, i);

            json_object_start(json1);
            json_string(json1, "aid"); json_integer(json1, cJSON_rsf_GetObjectItem(j_ch, "aid")->valuefloat);
            json_string(json1, "iid"); json_integer(json1, cJSON_rsf_GetObjectItem(j_ch, "iid")->valuefloat);
            json_string(json1, "status"); json_integer(json1, statuses[i]);
            json_object_end(json1);
            
            if (json1->error) {
                break;
            }
        }

        json_array_end(json1);
        json_object_end(json1); // response

        json_flush(json1);
        //json_buffer_free(json1);
        //free(json1);

        if (json1->error) {
            CLIENT_ERROR(context, "JSON");
        }
        
        client_send_chunk(NULL, 0, context);
    }

    free(statuses);
    cJSON_rsf_Delete(json);
}

void homekit_server_on_pairings(client_context_t *context, const byte *data, size_t size) {
    HOMEKIT_DEBUG_LOG("Pairings");
    DEBUG_HEAP();

    tlv_values_t *message = tlv_new();
    if (tlv_parse(data, size, message)) {
        CLIENT_ERROR(context, "TLV");
        tlv_free(message);
        send_tlv_error_response(context, 2, TLVError_Unknown);
        return;
    }

    TLV_DEBUG(message);

    int r;
    
    if (tlv_get_integer_value(message, TLVType_State, -1) != 1) {
        send_tlv_error_response(context, 2, TLVError_Unknown);
        tlv_free(message);
        return;
    }

    switch(tlv_get_integer_value(message, TLVType_Method, -1)) {
        case TLVMethod_AddPairing: {
            CLIENT_INFO(context, "Adding pairing");

            if (!(context->permissions & pairing_permissions_admin)) {
                CLIENT_ERROR(context, "Non-admin");
                send_tlv_error_response(context, 2, TLVError_Authentication);
                break;
            }

            tlv_t *tlv_device_identifier = tlv_get_value(message, TLVType_Identifier);
            if (!tlv_device_identifier) {
                CLIENT_ERROR(context, "No id");
                send_tlv_error_response(context, 2, TLVError_Unknown);
                break;
            }
            tlv_t *tlv_device_public_key = tlv_get_value(message, TLVType_PublicKey);
            if (!tlv_device_public_key) {
                CLIENT_ERROR(context, "No pub key");
                send_tlv_error_response(context, 2, TLVError_Unknown);
                break;
            }
            int device_permissions = tlv_get_integer_value(message, TLVType_Permissions, -1);
            if (device_permissions == -1) {
                CLIENT_ERROR(context, "No perm");
                send_tlv_error_response(context, 2, TLVError_Unknown);
                break;
            }

            ed25519_key *device_key = crypto_ed25519_new();
            r = crypto_ed25519_import_public_key(
                device_key, tlv_device_public_key->value, tlv_device_public_key->size
            );
            if (r) {
                CLIENT_ERROR(context, "Import pub key (%d)", r);
                crypto_ed25519_free(device_key);
                send_tlv_error_response(context, 2, TLVError_Unknown);
                break;
            }

            char *device_identifier = strndup(
                (const char *)tlv_device_identifier->value,
                tlv_device_identifier->size
            );
            
            pairing_t *pairing = homekit_storage_find_pairing(device_identifier);
            if (pairing) {
                if (!homekit_server->config->no_pairing_erase) {
                    size_t pairing_public_key_size = 0;
                    crypto_ed25519_export_public_key(pairing->device_key, NULL, &pairing_public_key_size);
                    
                    byte *pairing_public_key = malloc(pairing_public_key_size);
                    r = crypto_ed25519_export_public_key(pairing->device_key, pairing_public_key, &pairing_public_key_size);
                    if (r) {
                        CLIENT_ERROR(context, "Export pub key (%d)", r);
                        free(pairing_public_key);
                        pairing_free(pairing);
                        free(device_identifier);
                        crypto_ed25519_free(device_key);
                        send_tlv_error_response(context, 2, TLVError_Unknown);
                    }
                    
                    pairing_free(pairing);
                    
                    if (pairing_public_key_size != tlv_device_public_key->size ||
                        memcmp(tlv_device_public_key->value, pairing_public_key, pairing_public_key_size)) {
                        CLIENT_ERROR(context, "Pub key != given");
                        free(pairing_public_key);
                        free(device_identifier);
                        crypto_ed25519_free(device_key);
                        send_tlv_error_response(context, 2, TLVError_Unknown);
                    }
                    
                    free(pairing_public_key);
                    
                    r = homekit_storage_update_pairing(device_identifier, device_key, device_permissions);
                    if (r) {
                        CLIENT_ERROR(context, "Store (%d)", r);
                        free(device_identifier);
                        crypto_ed25519_free(device_key);
                        send_tlv_error_response(context, 2, TLVError_Unknown);
                        break;
                    }
                    
                    CLIENT_INFO(context, "Updated %s, %i", device_identifier, device_permissions);
                } else {
                    pairing_free(pairing);
                }
                
            } else {
                if (!homekit_storage_can_add_pairing()) {
                    CLIENT_ERROR(context, "Max peers");
                    free(device_identifier);
                    crypto_ed25519_free(device_key);
                    send_tlv_error_response(context, 2, TLVError_MaxPeers);
                    break;
                }

                r = homekit_storage_add_pairing(device_identifier, device_key, device_permissions);
                if (r) {
                    CLIENT_ERROR(context, "Store (%d)", r);
                    free(device_identifier);
                    crypto_ed25519_free(device_key);
                    send_tlv_error_response(context, 2, TLVError_Unknown);
                    break;
                }
                
                CLIENT_INFO(context, "Added %s, %i", device_identifier, device_permissions);

                HOMEKIT_NOTIFY_EVENT(homekit_server, HOMEKIT_EVENT_PAIRING_ADDED);
            }

            free(device_identifier);
            crypto_ed25519_free(device_key);

            tlv_values_t *response = tlv_new();
            tlv_add_integer_value(response, TLVType_State, 1, 2);

            send_tlv_response(context, response);

            break;
        }
        case TLVMethod_RemovePairing: {
            CLIENT_INFO(context, "Removing pairing");

            if (!(context->permissions & pairing_permissions_admin)) {
                CLIENT_ERROR(context, "Non-admin");
                send_tlv_error_response(context, 2, TLVError_Authentication);
                break;
            }

            tlv_t *tlv_device_identifier = tlv_get_value(message, TLVType_Identifier);
            if (!tlv_device_identifier) {
                CLIENT_ERROR(context, "No id");
                send_tlv_error_response(context, 2, TLVError_Unknown);
                break;
            }

            if (!homekit_server->config->no_pairing_erase) {
                char *device_identifier = strndup(
                                                  (const char *)tlv_device_identifier->value,
                                                  tlv_device_identifier->size
                                                  );
                
                pairing_t *pairing = homekit_storage_find_pairing(device_identifier);
                
                if (pairing) {
                    unsigned int is_admin = pairing->permissions & pairing_permissions_admin;
                    pairing_free(pairing);
                    
                    r = homekit_storage_remove_pairing(device_identifier);
                    if (r) {
                        CLIENT_ERROR(context, "Store (%d)", r);
                        free(device_identifier);
                        send_tlv_error_response(context, 2, TLVError_Unknown);
                        break;
                    }
                    
                    CLIENT_INFO(context, "Removed %s", device_identifier);
                    
                    HOMEKIT_NOTIFY_EVENT(homekit_server, HOMEKIT_EVENT_PAIRING_REMOVED);
                    
                    client_context_t *c = homekit_server->clients;
                    while (c) {
                        if (c->pairing_id == pairing->id) {
                            homekit_disconnect_client(c);
                        }
                        c = c->next;
                    }
                    
                    if (is_admin) {
                        // Removed pairing was admin,
                        // check if there any other admins left.
                        // If no admins left, enable pairing again
                        pairing_iterator_t *pairing_it = homekit_storage_pairing_iterator();
                        pairing_t *pairing;
                        while ((pairing = homekit_storage_next_pairing(pairing_it))) {
                            if (pairing->permissions & pairing_permissions_admin) {
                                break;
                            }
                            pairing_free(pairing);
                        };
                        free(pairing_it);
                        
                        if (!pairing) {
                            // No admins left, enable pairing again
                            homekit_server_on_reset(context);
                        } else {
                            pairing_free(pairing);
                        }
                    }
                } else {
                    CLIENT_INFO(context, "Not found %s", device_identifier);
                }
                
                free(device_identifier);
            }
            
            tlv_values_t *response = tlv_new();
            tlv_add_integer_value(response, TLVType_State, 1, 2);
            
            send_tlv_response(context, response);
            
            break;
        }
        case TLVMethod_ListPairings: {
            CLIENT_INFO(context, "List");
            
            if (!(context->permissions & pairing_permissions_admin)) {
                CLIENT_ERROR(context, "Non-admin");
                send_tlv_error_response(context, 2, TLVError_Authentication);
                break;
            }
            
            tlv_values_t *response = tlv_new();
            tlv_add_integer_value(response, TLVType_State, 1, 2);
            
            unsigned int first = true;
            unsigned int error = false;
            
            pairing_iterator_t *it = homekit_storage_pairing_iterator();
            pairing_t *pairing;
            
            size_t public_key_size = 0;
            
            while ((pairing = homekit_storage_next_pairing(it))) {
                crypto_ed25519_export_public_key(pairing->device_key, NULL, &public_key_size);
                byte *public_key = malloc(public_key_size);
                r = crypto_ed25519_export_public_key(pairing->device_key, public_key, &public_key_size);
                if (!r) {
                    if (!first) {
                        tlv_add_value(response, TLVType_Separator, NULL, 0);
                    }
                    
                    tlv_add_string_value(response, TLVType_Identifier, pairing->device_id);
                    tlv_add_value(response, TLVType_PublicKey, public_key, public_key_size);
                    tlv_add_integer_value(response, TLVType_Permissions, 1, pairing->permissions);
                    
                    first = false;
                    
                    CLIENT_INFO(context, "%s, %i", pairing->device_id, pairing->permissions);
                } else {
                    error = true;
                }
                
                free(public_key);
                pairing_free(pairing);
                
                if (error) {
                    break;
                }
            }
            
            free(it);

            if (error) {
                tlv_free(response);
                send_tlv_error_response(context, 2, TLVError_Unknown);
            } else {
                send_tlv_response(context, response);
            }
            
            break;
        }
        default: {
            send_tlv_error_response(context, 2, TLVError_Unknown);
            break;
        }
    }

    tlv_free(message);
}

void homekit_server_on_reset(client_context_t *context) {
    HOMEKIT_INFO("Reset");

    homekit_server_reset();
    send_204_response(context);

    vTaskDelay(2600 / portTICK_PERIOD_MS);

    sdk_system_restart();
}

#ifdef HOMEKIT_SERVER_ON_RESOURCE_ENABLE
void homekit_server_on_resource(client_context_t *context) {
    CLIENT_INFO(context, "Resource");
    DEBUG_HEAP();

    if (!homekit_server->config->on_resource) {
        send_404_response(context);
        return;
    }

    homekit_server->config->on_resource(context->body, context->body_length);
}
#endif

void homekit_server_on_prepare(client_context_t *context) {
    CLIENT_INFO(context, "Prepare");
    DEBUG_HEAP();

    send_json_error_response(context, 200, HAPStatus_Success);
}


int homekit_server_on_url(http_parser *parser, const char *data, size_t length) {
    client_context_t *context = (client_context_t*) parser->data;

    context->endpoint = HOMEKIT_ENDPOINT_UNKNOWN;
    if (parser->method == HTTP_GET) {
        if (!strncmp(data, "/accessories", length)) {
            context->endpoint = HOMEKIT_ENDPOINT_GET_ACCESSORIES;
        } else {
            const char url[] = "/characteristics";
            size_t url_len = sizeof(url) - 1;

            if (length >= url_len && !strncmp(data, url, url_len) &&
                    (data[url_len] == 0 || data[url_len] == '?'))
            {
                context->endpoint = HOMEKIT_ENDPOINT_GET_CHARACTERISTICS;
                if (data[url_len] == '?') {
                    char *query = strndup(data + url_len + 1, length - url_len - 1);
                    context->endpoint_params = query_params_parse(query);
                    free(query);
                }
            }
        }
    } else if (parser->method == HTTP_POST) {
        if (!strncmp(data, "/identify", length)) {
            context->endpoint = HOMEKIT_ENDPOINT_IDENTIFY;
        } else if (!strncmp(data, "/pair-setup", length)) {
            context->endpoint = HOMEKIT_ENDPOINT_PAIR_SETUP;
        } else if (!strncmp(data, "/pair-verify", length)) {
            context->endpoint = HOMEKIT_ENDPOINT_PAIR_VERIFY;
        } else if (!strncmp(data, "/pairings", length)) {
            context->endpoint = HOMEKIT_ENDPOINT_PAIRINGS;
#ifdef HOMEKIT_SERVER_ON_RESOURCE_ENABLE
        } else if (!strncmp(data, "/resource", length)) {
            context->endpoint = HOMEKIT_ENDPOINT_RESOURCE;
#endif
        }
    } else if (parser->method == HTTP_PUT) {
        if (!strncmp(data, "/characteristics", length)) {
            context->endpoint = HOMEKIT_ENDPOINT_UPDATE_CHARACTERISTICS;
        } else if (!strncmp(data, "/prepare", length)) {
            context->endpoint = HOMEKIT_ENDPOINT_PREPARE;
        }
    }
    
    if (context->endpoint == HOMEKIT_ENDPOINT_UNKNOWN) {
        HOMEKIT_ERROR("%s", http_method_str(parser->method));
        //char *url = strndup(data, length);
        //HOMEKIT_ERROR("URL %s", url);
        //free(url);
    }
    
    return 0;
}

int homekit_server_on_body(http_parser *parser, const char *data, size_t length) {
    client_context_t *context = parser->data;
    char* new_body = realloc(context->body, context->body_length + length + 1);
    if (!new_body) {
        CLIENT_ERROR(context, "Body");
        return -1;
    }
    context->body = new_body;
    memcpy(context->body + context->body_length, data, length);
    context->body_length += length;
    context->body[context->body_length] = 0;

    return 0;
}

int homekit_server_on_message_complete(http_parser *parser) {
    client_context_t *context = parser->data;
    
    switch(context->endpoint) {
        case HOMEKIT_ENDPOINT_PAIR_SETUP: {
            homekit_server_on_pair_setup(context, (const byte *)context->body, context->body_length);
            break;
        }
        case HOMEKIT_ENDPOINT_PAIR_VERIFY: {
            homekit_server_on_pair_verify(context, (const byte *)context->body, context->body_length);
            break;
        }
        case HOMEKIT_ENDPOINT_IDENTIFY: {
            if (context->encrypted || homekit_server->config->insecure) {
                homekit_server_on_identify(context);
            }
            break;
        }
        case HOMEKIT_ENDPOINT_GET_ACCESSORIES: {
            if (context->encrypted || homekit_server->config->insecure) {
                homekit_server_on_get_accessories(context);
            }
            break;
        }
        case HOMEKIT_ENDPOINT_GET_CHARACTERISTICS: {
            if (context->encrypted || homekit_server->config->insecure) {
                homekit_server_on_get_characteristics(context);
            }
            break;
        }
        case HOMEKIT_ENDPOINT_UPDATE_CHARACTERISTICS: {
            if (context->encrypted || homekit_server->config->insecure) {
                homekit_server_on_update_characteristics(context, (const byte *)context->body, context->body_length);
            }
            break;
        }
        case HOMEKIT_ENDPOINT_PAIRINGS: {
            if (context->encrypted || homekit_server->config->insecure) {
                homekit_server_on_pairings(context, (const byte *)context->body, context->body_length);
            }
            break;
        }
#ifdef HOMEKIT_SERVER_ON_RESOURCE_ENABLE
        case HOMEKIT_ENDPOINT_RESOURCE: {
            if (context->encrypted || homekit_server->config->insecure) {
                homekit_server_on_resource(context);
            }
            break;
        }
#endif
        case HOMEKIT_ENDPOINT_PREPARE: {
            if (context->encrypted || homekit_server->config->insecure) {
                homekit_server_on_prepare(context);
            }
            break;
        }
        case HOMEKIT_ENDPOINT_UNKNOWN: {
            HOMEKIT_DEBUG_LOG("Unknown");
            send_404_response(context);
            break;
        }
    }

    if (context->endpoint_params) {
        query_params_free(context->endpoint_params);
        context->endpoint_params = NULL;
    }

    if (context->body) {
        free(context->body);
        context->body = NULL;
        context->body_length = 0;
    }

    return 0;
}


static http_parser_settings homekit_http_parser_settings = {
    .on_url = homekit_server_on_url,
    .on_body = homekit_server_on_body,
    .on_message_complete = homekit_server_on_message_complete,
};

static inline void homekit_client_process(client_context_t *context) {
    int data_len = read(context->socket,
                        homekit_server->data,
                        sizeof(homekit_server->data)
                        );
    
    if (data_len > 0) {
        CLIENT_DEBUG(context, "Got %d incomming data", data_len);
        byte *payload = (byte*) homekit_server->data;
        size_t payload_size = (size_t) data_len;
        CLIENT_DEBUG(context, "Received Payload:\n%s", (char*) payload);
        
        size_t decrypted_size = sizeof(homekit_server->data) - 16 - 2;
        
        if (context->encrypted) {
            CLIENT_DEBUG(context, "Decrypting data");
            
            int r = client_decrypt(context, homekit_server->data, data_len, homekit_server->data + 2, &decrypted_size);
            if (r < 0) {
                CLIENT_ERROR(context, "Client data");
                return;
            }
            
            CLIENT_DEBUG(context, "Decrypt %d bytes", decrypted_size);
            
            payload = homekit_server->data + 2;
            payload_size = decrypted_size;
            
            if (payload_size) {
                print_binary("Decrypt data", payload, payload_size);
            }
        }
        
        http_parser_execute(context->parser, &homekit_http_parser_settings,
                            (char*) payload, payload_size
                            );
        
    } else if (data_len == 0) {
        CLIENT_INFO(context, "Closing");
        homekit_disconnect_client(context);
        
    } else {    // if (data_len < 0)
        if (errno != EAGAIN) {
            CLIENT_ERROR(context, "Socket (%d)", errno);
            homekit_disconnect_client(context);
        }
    }
    
    CLIENT_DEBUG(context, "Finished processing");
}


void homekit_server_close_client(client_context_t *context) {
    FD_CLR(context->socket, &homekit_server->fds);
    if (homekit_server->client_count > 0) {
        homekit_server->client_count--;
    }
    
    close(context->socket);
    
    CLIENT_INFO(context, "Closed %i/%i", homekit_server->client_count, homekit_server->config->max_clients);
    
    if (homekit_server->pairing_context && homekit_server->pairing_context->client == context) {
        pairing_context_free(homekit_server->pairing_context);
        homekit_server->pairing_context = NULL;
    }
    
    homekit_accessories_clear_notify_subscriptions(homekit_server->config->accessories, context);
    
    HOMEKIT_NOTIFY_EVENT(homekit_server, HOMEKIT_EVENT_CLIENT_DISCONNECTED);

    client_context_free(context);
}


static inline void homekit_server_accept_client() {
    int s = accept(homekit_server->listen_fd, (struct sockaddr*) NULL, (socklen_t*) NULL);
    if (s < 0) {
        HOMEKIT_ERROR("Socket");
        return;
    }

    char address_buffer[INET_ADDRSTRLEN];

    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    if (getpeername(s, (struct sockaddr *)&addr, &addr_len) == 0) {
        inet_ntop(AF_INET, &addr.sin_addr, address_buffer, sizeof(address_buffer));
    } else {
        HOMEKIT_ERROR("[%d] New ?:%d", s, addr.sin_port);
        close(s);
        return;
    }
    
    client_context_t* new_context = client_context_new();
    
    const uint_fast32_t free_heap = xPortGetFreeHeapSize();
    
    if (new_context) {
        /*
        const int nodelay = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
        */
        
        const struct timeval sndtimeout = { 3, 0 };
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &sndtimeout, sizeof(sndtimeout));
        
        const struct timeval rcvtimeout = { 13, 0 };
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeout, sizeof(rcvtimeout));
        
        const int keepalive = 0;
        setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));

        new_context->socket = s;
        new_context->next = homekit_server->clients;

        homekit_server->clients = new_context;

        FD_SET(s, &homekit_server->fds);
        homekit_server->client_count++;
        if (s > homekit_server->max_fd) {
            homekit_server->max_fd = s;
        }
        
        HOMEKIT_INFO("[%i] New %s:%d %i/%i HEAP %"HK_LONGINT_F, s, address_buffer, addr.sin_port, homekit_server->client_count, homekit_server->config->max_clients, free_heap);
        
        HOMEKIT_NOTIFY_EVENT(homekit_server, HOMEKIT_EVENT_CLIENT_CONNECTED);
        
    } else {
        close(s);
        
        HOMEKIT_ERROR("[%i] DRAM %s:%d %i/%i HEAP %"HK_LONGINT_F, s, address_buffer, addr.sin_port, homekit_server->client_count, homekit_server->config->max_clients, free_heap);
    }
    
    if (homekit_server->client_count >= homekit_server->config->max_clients || !new_context) {
        homekit_remove_oldest_client();
    }
}

void homekit_characteristic_notify(homekit_characteristic_t *ch) {
    if (homekit_server) {
        notification_t* notification = homekit_server->notifications;
        if (notification) {
            for (;;) {
                if (notification->ch == ch) {
                    return;
                }
                
                if (notification->next) {
                    notification = notification->next;
                } else {
                    break;
                }
            }
        }
        
        notification_t* notification_new = calloc(1, sizeof(notification_t));
        
        notification_new->ch = ch;
        
        if (notification) {
            notification->next = notification_new;
        } else {
            homekit_server->notifications = notification_new;
        }
    }
}

static inline void IRAM homekit_server_process_notifications() {
    notification_t *notifications = homekit_server->notifications;
    homekit_server->notifications = NULL;
    
    client_context_t *context = homekit_server->clients;
    while (context) {
        notification_t *notification = notifications;
        while (notification) {
            if (homekit_characteristic_has_notify_subscription(notification->ch, context)) {
                CLIENT_INFO(context, "Send Ev");
                DEBUG_HEAP();
                
                json_stream* json = &homekit_server->json;
                json_init(json, context);
                
                byte http_headers[] =
                    "EVENT/1.0 200 OK\r\n"
                    "Content-Type: application/hap+json\r\n"
                    "Transfer-Encoding: chunked\r\n\r\n";
                
                if (client_send(context, http_headers, sizeof(http_headers) - 1) < 0) {
                    json->error = true;
                }
                
                json_object_start(json);
                json_string(json, "characteristics"); json_array_start(json);
                
                notification = notifications;
                while (notification) {
                    json_object_start(json);
                    write_characteristic_json(json, context, notification->ch, 0, &notification->ch->value, 0);
                    json_object_end(json);
                    
                    if (json->error) {
                        break;
                    }
                    
                    notification = notification->next;
                }
                
                json_array_end(json);
                json_object_end(json);
                
                json_flush(json);
                
                if (json->error) {
                    CLIENT_ERROR(context, "JSON");
                }
                
                client_send_chunk(NULL, 0, context);
                
                break;
            }

            notification = notification->next;
        }
        
        context = context->next;
    }
    
    // Remove sent notifications
    while (notifications) {
        notification_t* notification_old = notifications;
        notifications = notifications->next;
        free(notification_old);
    }
}

static inline void homekit_server_close_clients() {
    if (homekit_server->pending_close) {
        homekit_server->pending_close = false;
        
        int max_fd = homekit_server->listen_fd;

        client_context_t head;
        head.next = homekit_server->clients;

        client_context_t *context = &head;
        while (context->next) {
            client_context_t *tmp = context->next;

            if (tmp->disconnect) {
                context->next = tmp->next;
                homekit_server_close_client(tmp);
            } else {
                if (tmp->socket > max_fd)
                    max_fd = tmp->socket;

                context = tmp;
            }
        }
        
        homekit_server->clients = head.next;
        homekit_server->max_fd = max_fd;
    }
}

static void IRAM homekit_run_server() {
    HOMEKIT_DEBUG_LOG("Starting HTTP server");
    
    struct sockaddr_in serv_addr;
    homekit_server->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    memset(&serv_addr, '0', sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(PORT);
    bind(homekit_server->listen_fd, (struct sockaddr*)&serv_addr, sizeof(serv_addr));
    
    /*
    int opt = lwip_fcntl(homekit_server->listen_fd, F_GETFL, 0);
    if (opt >= 0) {
        opt |= O_NONBLOCK;
        if (lwip_fcntl(homekit_server->listen_fd, F_SETFL, opt) == -1) {
            HOMEKIT_ERROR("NonBlock Socket");
        }
    }
    */
    
    listen(homekit_server->listen_fd, 10);
    
    FD_SET(homekit_server->listen_fd, &homekit_server->fds);
    homekit_server->max_fd = homekit_server->listen_fd;
    
    struct timeval timeout = { 0, 80000 }; /* 0.08 seconds timeout (orig: 1s) */
    int triggered_nfds;
    fd_set read_fds;
    
    for (;;) {
        memcpy(&read_fds, &homekit_server->fds, sizeof(read_fds));
        
        triggered_nfds = select(homekit_server->max_fd + 1, &read_fds, NULL, NULL, &timeout);
        if (triggered_nfds > 0) {
            if (FD_ISSET(homekit_server->listen_fd, &read_fds)) {
                homekit_server_accept_client();
                triggered_nfds--;
            }
            
            client_context_t *context = homekit_server->clients;
            while (context && triggered_nfds) {
                if (FD_ISSET(context->socket, &read_fds)) {
                    homekit_client_process(context);
                    triggered_nfds--;
                }
                
                context = context->next;
            }
            
            if (homekit_low_dram()) {
                homekit_remove_oldest_client();
            }
            
            homekit_server_close_clients();
        }
        
        if (homekit_server->notifications) {
            homekit_server_process_notifications();
        }
    }
    
    //server_free();
}

void homekit_setup_mdns() {
    homekit_accessory_t *accessory = homekit_server->config->accessories[0];
    homekit_service_t *accessory_info =
        homekit_service_by_type(accessory, HOMEKIT_SERVICE_ACCESSORY_INFORMATION);
    
    /*
    if (!accessory_info) {
        HOMEKIT_ERROR("Accessory declaration: no Acc Information service");
        return;
    }
     */

    homekit_characteristic_t *name =
        homekit_service_characteristic_by_type(accessory_info, HOMEKIT_CHARACTERISTIC_NAME);
    /*
    if (!name) {
        HOMEKIT_ERROR("Accessory declaration: no Name ch in AccessoryInfo service");
        return;
    }
     */

    homekit_characteristic_t *model =
        homekit_service_characteristic_by_type(accessory_info, HOMEKIT_CHARACTERISTIC_MODEL);
    /*
    if (!model) {
        HOMEKIT_ERROR("Accessory declaration: no Model ch in AccessoryInfo service");
        return;
    }
     */
    
    homekit_mdns_configure_init(name->value.string_value, PORT);
    
    // accessory model name (required)
    homekit_mdns_add_txt("md", "%s", model->value.string_value);
    
    // protocol version (required)
    homekit_mdns_add_txt("pv", "1.1");
    
    // device ID (required)
    // should be in format XX:XX:XX:XX:XX:XX, otherwise devices will ignore it
    homekit_mdns_add_txt("id", "%s", homekit_server->accessory_id);
    
    // current configuration number (required)
    homekit_mdns_add_txt("c#", "%d", homekit_server->config->config_number);
    
    // current state number (required)
    int32_t current_state_number = 1;
    if (!homekit_server->paired) {
        sysparam_set_int32("cs", 0);
    } else {
        sysparam_get_int32("cs", &current_state_number);
        current_state_number++;
        if (current_state_number > 65535) {
            current_state_number = 1;
        }
        sysparam_set_int32("cs", current_state_number);
    }
    homekit_mdns_add_txt("s#", "%d", current_state_number);
    
    // feature flags (required if non-zero)
    //   bit 0 - supports HAP pairing. required for all HomeKit accessories
    //   bits 1-7 - reserved
    //homekit_mdns_add_txt("ff", "0");
    
    // status flags
    //   bit 0 - not paired
    //   bit 1 - not configured to join WiFi
    //   bit 2 - problem detected on accessory
    //   bits 3-7 - reserved
    homekit_mdns_add_txt("sf", "%d", (homekit_server->paired) ? 0 : 1);
    
    // accessory category identifier
    homekit_mdns_add_txt("ci", "%d", homekit_server->config->category);
    
    homekit_server->config->setup_id = strdup("JOSE");
    
    HOMEKIT_DEBUG_LOG("Setup ID: %s", homekit_server->config->setup_id);
    
    size_t data_size = strlen(homekit_server->config->setup_id) + strlen(homekit_server->accessory_id) + 1;
    char *data = malloc(data_size);
    snprintf(data, data_size, "%s%s", homekit_server->config->setup_id, homekit_server->accessory_id);
    //data[data_size - 1] = 0;
    
    unsigned char shaHash[SHA512_DIGEST_SIZE];
    wc_Sha512Hash((const unsigned char*) data, data_size - 1, shaHash);
    
    free(data);
    free(homekit_server->config->setup_id);
    
    unsigned char encodedHash[9];
    memset(encodedHash, 0, sizeof(encodedHash));
    
    word32 len = sizeof(encodedHash);
    Base64_Encode_NoNl((const unsigned char *)shaHash, 4, encodedHash, &len);
    
    homekit_mdns_add_txt("sh", "%s", encodedHash);
    
    homekit_mdns_configure_finalize(homekit_server->config->mdns_ttl, homekit_server->config->mdns_ttl_period);
}

char *homekit_accessory_id_generate() {
    char *accessory_id = malloc(18);

    byte buf[6];
    homekit_random_fill(buf, sizeof(buf));

    snprintf(accessory_id, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             buf[0], buf[1], buf[2], buf[3], buf[4], buf[5]);

    HOMEKIT_INFO("HK ID %s", accessory_id);
    
    return accessory_id;
}

/*
ed25519_key *homekit_accessory_key_generate() {
    ed25519_key *key = crypto_ed25519_generate();
    if (!key) {
        HOMEKIT_ERROR("HK New Key");
        return NULL;
    }

    HOMEKIT_INFO("HK New Key");

    return key;
}
*/

void homekit_server_task(void *args) {
    vTaskDelay(500 / portTICK_PERIOD_MS);
    
    homekit_mdns_init();
    
    homekit_setup_mdns();

    HOMEKIT_NOTIFY_EVENT(homekit_server, HOMEKIT_EVENT_SERVER_INITIALIZED);

    homekit_run_server();

    //vTaskDelete(NULL);
}

#define ISDIGIT(x)      isdigit((unsigned char)(x))
#define ISBASE36(x)     (isdigit((unsigned char)(x)) || (x >= 'A' && x <= 'Z'))

void homekit_server_init(homekit_server_config_t *config) {
    HOMEKIT_INFO("Start HK");
    
    homekit_accessories_init(config->accessories);
    
    homekit_server = server_new();
    homekit_server->config = config;
    
    if (homekit_server->config->max_clients == 0) {
        homekit_server->config->max_clients = HOMEKIT_MAX_CLIENTS_DEFAULT;
    }

    int r = homekit_storage_init();

    if (r == 0) {
        homekit_server->accessory_id = homekit_storage_load_accessory_id();
        homekit_server->accessory_key = homekit_storage_load_accessory_key();
    }
    if (!homekit_server->accessory_id || !homekit_server->accessory_key) {
        homekit_server->accessory_id = homekit_accessory_id_generate();
        homekit_storage_save_accessory_id(homekit_server->accessory_id);
        
        //homekit_server->accessory_key = homekit_accessory_key_generate();
        homekit_server->accessory_key = crypto_ed25519_generate();
        homekit_storage_save_accessory_key(homekit_server->accessory_key);
    } else {
        HOMEKIT_INFO("HK ID: %s", homekit_server->accessory_id);
    }
    
    if (!homekit_server->config->re_pair) {
        pairing_iterator_t *pairing_it = homekit_storage_pairing_iterator();
        pairing_t *pairing = NULL;
        
        while ((pairing = homekit_storage_next_pairing(pairing_it))) {
            if (pairing->permissions & pairing_permissions_admin) {
                HOMEKIT_INFO("Found %s", pairing->device_id);
                pairing_free(pairing);
                homekit_server->paired = true;
                break;
            }
            pairing_free(pairing);
        }
        
        free(pairing_it);
    }
    
    unsigned int server_task_stack = SERVER_TASK_STACK_PAIR;

#ifndef ESP_PLATFORM
    if (homekit_server->paired) {
        server_task_stack = SERVER_TASK_STACK_NORMAL;
    }
#endif
    
    if (xTaskCreate(homekit_server_task, "HK", server_task_stack, NULL, SERVER_TASK_PRIORITY, NULL) != pdPASS) {
        ERROR("New HK");
    }
}

void homekit_server_reset() {
    homekit_storage_reset();
}

void homekit_remove_extra_pairing(const unsigned int last_keep) {
    homekit_storage_remove_extra_pairing(last_keep);
}

unsigned int homekit_pairing_count() {
    return homekit_storage_pairing_count();
}

void homekit_mdns_announce_stop() {
    homekit_port_mdns_announce_stop();
}

void homekit_mdns_announce_start() {
    homekit_port_mdns_announce_start();
}

bool homekit_is_paired() {
    return homekit_server->paired;
}

int homekit_get_accessory_id(char *buffer, size_t size) {
    char *accessory_id = homekit_storage_load_accessory_id();
    if (!accessory_id)
        return -2;

    if (size < strlen(accessory_id) + 1)
        return -1;

    strncpy(buffer, accessory_id, size);

    free(accessory_id);

    return 0;
}

bool homekit_is_pairing() {
    if (homekit_server) {
        return homekit_server->is_pairing;
    }
    
    return false;
}
