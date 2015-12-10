#ifndef LUA_SERDE_H
#define LUA_SERDE_H

#include <lua.h>
#include <stdint.h>

int lua_serde_serialize(lua_State *L, uint8_t *buffer, size_t len);

int lua_serde_deserialize(lua_State *L, const uint8_t *buffer, size_t len);

#endif
