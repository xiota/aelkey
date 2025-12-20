#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include <lua.hpp>

#include "device_input.h"

void ensure_gatt_initialized();

InputCtx attach_gatt_device(const InputDecl &decl);
void detach_gatt_device(InputCtx &ctx);
void dispatch_gatt(lua_State *L);

std::string match_gatt_device(
    const InputDecl &decl,
    std::vector<std::string> *found_characteristics = nullptr
);

bool gatt_read_characteristic(const std::string &char_path, std::vector<uint8_t> &out_data);

bool gatt_write_characteristic(
    const std::string &char_path,
    const uint8_t *data,
    size_t len,
    bool with_resp
);

bool gatt_enumerate_characteristics(
    const std::string &device_path,
    std::unordered_map<std::string, std::string> &uuid_to_path
);

bool gatt_resolve_characteristic_for_uuid(
    InputCtx *ctx,
    const std::string &full_uuid,
    std::string &out_char_path
);
