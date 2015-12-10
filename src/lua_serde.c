#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define LUA_SERDE_BUFFER_SIZE 1024
#define LUA_SERDE_ERROR_SIZE 256

enum lua_serde_types {
	LUA_SERDE_NIL = 0,
	LUA_SERDE_BOOL,
	LUA_SERDE_NUMBER,
	LUA_SERDE_STRING,
	LUA_SERDE_TABLE
};

static int
serialize_internal(lua_State *L, uint8_t **buffer, uint8_t *end)
{
	uint8_t *pos = *buffer;
	
	int type = lua_type(L, -1);

	switch (type) {
		case LUA_TNIL:
		{
			if (pos + sizeof(uint8_t) > end) {
				return -1;
			}
			*pos++ = LUA_SERDE_NIL;
			break;
		}
		case LUA_TBOOLEAN:
		{
			if (pos + 2 * sizeof(uint8_t) > end) {
			   return -1;
			}	   
			*pos++ = LUA_SERDE_BOOL;
			*pos++ = (uint8_t)lua_toboolean(L, -1);
			break;
		}
		case LUA_TNUMBER:
		{
			if (pos + sizeof(uint8_t) + sizeof(double) > end) {
				return -1;
			}
			*pos++ = LUA_SERDE_NUMBER;
			double val = lua_tonumber(L, -1);
			memcpy(pos, &val, sizeof(val));
			pos += sizeof(val);
			break;
		}
		case LUA_TSTRING:
		{
			size_t len;
			const char *str = lua_tolstring(L, -1, &len);
			if (pos + sizeof(uint8_t) + sizeof(len) + len > end) {
				return -1;
			}
			*pos++ = LUA_SERDE_STRING;
			memcpy(pos, &len, sizeof(len));
			pos += sizeof(len);
			memcpy(pos, str, len);
			pos += len;
			break;
		}
		case LUA_TTABLE:
		{
			if (pos + sizeof(uint8_t) > end) {
				return -1;
			}
			*pos++ = LUA_SERDE_TABLE;

			/* leave room for table length */
			size_t len = 0;
			uint8_t *lenidx = pos;
			pos += sizeof(len);

			/* need to do this to handle sub-tables */
			lua_pushvalue(L, -1);

			/* first key */
			lua_pushnil(L);
			while (lua_next(L, -2) != 0) {
				/* increment array length */
				len++;

				/* serialize key */
				if (lua_isnumber(L, -2) == 1) {
					if (pos + sizeof(uint8_t) + sizeof(double) > end) {
						return -1;
					}
					*pos++ = LUA_SERDE_NUMBER;
					double keyval = lua_tonumber(L, -2);
					memcpy(pos, &keyval, sizeof(keyval));
					pos += sizeof(keyval);
				} else if (lua_isstring(L, -2) == 1) {
					size_t keylen;
					const char *str = lua_tolstring(L, -2, &keylen);
					if (pos + sizeof(uint8_t) + sizeof(keylen) + keylen > end) {
						return -1;
					}
					*pos++ = LUA_SERDE_STRING;
					memcpy(pos, &keylen, sizeof(keylen));
					pos += sizeof(keylen);
					memcpy(pos, str, keylen);
					pos += keylen;
				} else {
					return -9;
				}

				/* recursively serialize value */
				int r = serialize_internal(L, &pos, end);
				if (r != 0) {
					return r;
				}
				lua_pop(L, 1);
			}
			lua_pop(L, 1);

			/* write array length */
			memcpy(lenidx, &len, sizeof(len));
			break;
		}
		default:
			return -9;
	}

	*buffer = pos;
	return 0;
}

static int
deserialize_internal(lua_State *L, const uint8_t **buffer, const uint8_t *end)
{
	const uint8_t *pos = *buffer;

	if (pos + sizeof(uint8_t) > end) {
		return -1;
	}
	uint8_t type = *pos++;

	switch (type) {
		case LUA_SERDE_NIL:
			lua_pushnil(L);
			break;
		case LUA_SERDE_BOOL:
		{
			if (pos + sizeof(uint8_t) > end) {
				return -1;
			}
			uint8_t b = *pos++;
			lua_pushboolean(L, (int)b);
			break;
		}
		case LUA_SERDE_NUMBER:
		{
			if (pos + sizeof(double) > end) {
				return -1;
			}
			double val;
			memcpy(&val, pos, sizeof(val));
			pos += sizeof(val);
			lua_pushnumber(L, val);
			break;
		}
		case LUA_SERDE_STRING:
		{
			if (pos + sizeof(size_t) > end) {
				return -1;
			}
			size_t len;
			memcpy(&len, pos, sizeof(len));
			pos += sizeof(len);
			if (pos + len > end) {
				return -1;
			}
			lua_pushlstring(L, (const char *)pos, len);
			pos += len;
			break;
		}
		case LUA_SERDE_TABLE:
		{
			if (pos + sizeof(size_t) > end) {
				return -1;
			}
			size_t len;
			memcpy(&len, pos, sizeof(len));
			pos += sizeof(len);
			if (pos + len > end) {
				return -1;
			}

			lua_newtable(L);

			size_t cnt = 0;
			while (cnt < len && pos < end) {
				cnt++;

				/* deserialize key */
				uint8_t keytype = *pos++;

				switch (keytype) {
					case LUA_SERDE_NUMBER:
						if (pos + sizeof(double) > end) {
							return -1;
						}
						double keyval;
						memcpy(&keyval, pos, sizeof(keyval));
						pos += sizeof(keyval);
						lua_pushnumber(L, keyval);
						break;
					case LUA_SERDE_STRING:
						if (pos + sizeof(size_t) > end) {
							return -1;
						}
						size_t keylen;
						memcpy(&keylen, pos, sizeof(keylen));
						pos += sizeof(keylen);
						if (pos + keylen > end) {
							return -1;
						}
						lua_pushlstring(L, (const char *)pos, keylen);
						pos += keylen;
						break;
					default:
						return -9;
				}

				/* deserialize value */
				deserialize_internal(L, &pos, end);

				/* set key, value - pops both */
				lua_settable(L, -3);
			}

			break;
		}
		default:
			return -9;
	}

	*buffer = pos;
	return 0;
}

static int
serialize_lua(lua_State *L)
{
	uint8_t buffer[LUA_SERDE_BUFFER_SIZE];
	uint8_t *pos = buffer;

	lua_pushvalue(L, 1);
	int r = serialize_internal(L, &pos, pos + sizeof(buffer));
	if (r != 0) {
		if (r == -1) {
			lua_pushstring(L, "exceeded buffer size");
		} else {
			lua_pushstring(L, "unsupported type encountered");
		}
		lua_error(L);
	}
	lua_pop(L, 1);

	lua_pushlstring(L, (const char *)buffer, pos - buffer);
	return 1;
}

static int
deserialize_lua(lua_State *L)
{
	if (lua_isstring(L, 1) == 0) {
		lua_pushstring(L, "input must be a string");
		lua_error(L);
	}

	size_t len;
	const uint8_t *start = (const uint8_t *)lua_tolstring(L, -1, &len);
	const uint8_t *pos = start;

	int r = deserialize_internal(L, &pos, pos + len);
	if (r != 0) {
		if (r == -1) {
			lua_pushstring(L, "exceeded buffer size");
		} else {
			char err[LUA_SERDE_ERROR_SIZE];
			sprintf(err, "invalid type encountered at byte %ld", pos - start);
			lua_pushstring(L, err);
		}
		lua_error(L);
	}

	return 1;
}

static const struct luaL_Reg serdelib[] = {
	{ "serialize", serialize_lua },
	{ "deserialize", deserialize_lua },
	{ NULL, NULL }
};

int luaopen_serde(lua_State *L) {
	luaL_newlib(L, serdelib);
	return 1;
}

/* serialize element at top of the stack to supplied buffer */
int
lua_serde_serialize(lua_State *L, uint8_t *buffer, size_t len)
{
	return serialize_internal(L, &buffer, buffer + len);
}

/* deserialize supplied buffer and push result onto stack */
int
lua_serde_deserialize(lua_State *L, const uint8_t *buffer, size_t len)
{
	return deserialize_internal(L, &buffer, buffer + len);
}
