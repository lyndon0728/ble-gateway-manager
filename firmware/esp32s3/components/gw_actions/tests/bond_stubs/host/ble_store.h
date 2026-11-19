#pragma once
#include <stdint.h>
typedef struct { uint8_t type,val[6]; } ble_addr_t;
struct ble_store_key_sec { ble_addr_t peer_addr; };
struct ble_store_value_sec {
    ble_addr_t peer_addr;
    uint8_t key_size; uint16_t ediv; uint64_t rand_num;
    uint8_t ltk[16],irk[16],csrk[16];
    uint8_t ltk_present,irk_present,csrk_present,authenticated,sc;
    uint32_t sign_counter;
};
#define BLE_STORE_OBJ_TYPE_OUR_SEC 1
#define BLE_STORE_OBJ_TYPE_PEER_SEC 2
int ble_store_read_our_sec(const struct ble_store_key_sec *,struct ble_store_value_sec *);
int ble_store_read_peer_sec(const struct ble_store_key_sec *,struct ble_store_value_sec *);
int ble_store_util_count(int,int *);
