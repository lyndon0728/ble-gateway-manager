#pragma once
#include "cJSON.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
typedef int esp_err_t;
enum {
  ESP_OK,
  ESP_FAIL,
  ESP_ERR_NO_MEM,
  ESP_ERR_INVALID_ARG,
  ESP_ERR_INVALID_STATE,
  ESP_ERR_INVALID_SIZE,
  ESP_ERR_NOT_FOUND,
  ESP_ERR_NOT_SUPPORTED,
  ESP_ERR_TIMEOUT,
  ESP_ERR_INVALID_RESPONSE,
  ESP_ERR_NOT_FINISHED
};
enum { GW_BUSY = 3 };
typedef void *SemaphoreHandle_t;
typedef void *QueueHandle_t;
#define pdTRUE 1
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(n) (n)
SemaphoreHandle_t xSemaphoreCreateMutex(void);
int xSemaphoreTake(SemaphoreHandle_t, unsigned);
void xSemaphoreGive(SemaphoreHandle_t);
QueueHandle_t xQueueCreate(unsigned, unsigned);
int xQueueSend(QueueHandle_t, const void *, unsigned);
int xQueueReceive(QueueHandle_t, void *, unsigned);
int xTaskCreate(void (*)(void *), const char *, unsigned, void *, unsigned,
                void *);
void vTaskDelay(unsigned);
uint64_t gw_core_mono_ms(void);
bool gw_core_time_bounds(uint64_t *, uint64_t *);
esp_err_t gw_factory_get_string(const char *, char *, size_t);
esp_err_t gw_factory_get_blob(const char *, void *, size_t *);
esp_err_t gw_store_put_json(const char *, const cJSON *);
cJSON *gw_store_get_json(const char *);
esp_err_t gw_store_get(const char *, void *, size_t *);
cJSON *gw_capture_peripheral_peek(void);
esp_err_t gw_capture_peripheral_ack(const char *);
void gw_capture_esp_boot(uint8_t[16]);
bool gw_nrf_identity(uint8_t[16], uint32_t *, char[64]);
esp_err_t gw_nrf_command(uint8_t, const void *, size_t, uint8_t *, void *,
                         size_t *, int64_t);
typedef esp_err_t (*gw_ble_peripheral_receiver)(uint32_t, const uint8_t *,
                                                size_t, uint16_t, void *);
void gw_ble_peripheral_set_receiver(gw_ble_peripheral_receiver, void *);
bool gw_ble_peripheral_identity(uint32_t, uint8_t *, uint8_t *, uint8_t *,
                                uint8_t *, uint16_t *);
esp_err_t gw_ble_peripheral_send(uint32_t, uint64_t, const uint8_t *, size_t);
esp_err_t gw_ble_peripheral_close(uint32_t);
esp_err_t gw_transport_json(const char *, const char *, const cJSON *,
                            const char *, uint64_t, cJSON **);
void esp_fill_random(void *, size_t);
uint16_t gw_get_u16(const uint8_t *);
uint32_t gw_get_u32(const uint8_t *);
uint64_t gw_get_u64(const uint8_t *);
void gw_put_u16(uint8_t *, uint16_t);
void gw_put_u32(uint8_t *, uint32_t);
void gw_put_u64(uint8_t *, uint64_t);
void mbedtls_platform_zeroize(void *, size_t);
int mbedtls_sha256(const unsigned char *, size_t, unsigned char[32], int);
int mbedtls_base64_decode(unsigned char *, size_t, size_t *,
                          const unsigned char *, size_t);
int mbedtls_base64_encode(unsigned char *, size_t, size_t *,
                          const unsigned char *, size_t);
typedef void *protocomm_security_handle_t;
typedef struct {
  const char *salt;
  uint16_t salt_len;
  const char *verifier;
  uint16_t verifier_len;
} protocomm_security2_params_t;
typedef struct {
  int ver;
  uint8_t patch_ver;
  esp_err_t (*init)(void **);
  esp_err_t (*cleanup)(void *);
  esp_err_t (*new_transport_session)(void *, uint32_t);
  esp_err_t (*close_transport_session)(void *, uint32_t);
  esp_err_t (*security_req_handler)(void *, const void *, uint32_t,
                                    const uint8_t *, ssize_t, uint8_t **,
                                    ssize_t *, void *);
  esp_err_t (*encrypt)(void *, uint32_t, const uint8_t *, ssize_t, uint8_t **,
                       ssize_t *);
  esp_err_t (*decrypt)(void *, uint32_t, const uint8_t *, ssize_t, uint8_t **,
                       ssize_t *);
} protocomm_security_t;
extern const protocomm_security_t protocomm_security2;
typedef struct {
  size_t len;
  uint8_t *data;
} Blob;
typedef struct {
  Blob client_pubkey, client_username;
} Sec2Command0;
typedef struct {
  Blob client_proof;
} Sec2Command1;
typedef struct {
  int msg, payload_case;
  Sec2Command0 *sc0;
  Sec2Command1 *sc1;
} Sec2Payload;
typedef struct {
  int sec_ver, proto_case;
  Sec2Payload *sec2;
} SessionData;
enum {
  SEC_SCHEME_VERSION__SecScheme2 = 2,
  SESSION_DATA__PROTO_SEC2 = 12,
  SEC2_MSG_TYPE__S2Session_Command0 = 0,
  SEC2_MSG_TYPE__S2Session_Command1 = 2,
  SEC2_PAYLOAD__PAYLOAD_SC0 = 20,
  SEC2_PAYLOAD__PAYLOAD_SC1 = 22
};
SessionData *session_data__unpack(void *, size_t, const uint8_t *);
void session_data__free_unpacked(SessionData *, void *);
