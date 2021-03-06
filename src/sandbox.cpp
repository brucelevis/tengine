#include "sandbox.hpp"

#include "context.hpp"
#include "timer.hpp"
#include "logger.hpp"

#ifdef LUA_JIT
#include "c-api/compat-5.3.h"
#else
#include "lua.hpp"
#endif // LUA_JIT

#ifdef LUA_JIT
#define ltablib_c
#include "lstrlib.c"
#include "ltablib.c"
#include "lutf8lib.c"
#endif

#include "sandbox_core.cpp"
#include "sandbox_timer.cpp"
#include "sandbox_logger.cpp"
#include "sandbox_server.cpp"
#include "sandbox_udp.cpp"
#include "sandbox_channel.cpp"
#include "sandbox_network.cpp"
#include "sandbox_mysql.cpp"
#include "sandbox_redis.cpp"
#include "sandbox_watchdog.cpp"

int luaopen_tengine(lua_State *L)
{
	luaL_checkversion(L);

	luaL_Reg l[] = {
		{ "start", start },
		{ "query", query },
		{ "register_name", register_name },
		{ "send", send },
		{ "dispatch", dispatch },
		{ "timer", timer },
		{ "log", log },
		{ "thread_id", thread_id },
		{ "now", now },
		{ "micronow", micro_now },
		{ "nanonow", nano_now },

		{ "systeminfo", system_info },
		{ "processinfo", process_info },

		{ "http", http },
		{ "web", web },
		{ "webserver", webserver },
		{ "server", server },
		{ "udp_server", udp_server },
		{ "channel", channel },
		{ "udp_channel", udp_channel },
		{ "udp_sender", udp_sender },
		{ "mysql", mysql },
		{ "announce", announcer },
		{ "files", files },
        { "redis", redis },
		{ "watchdog", watchdog },

		{ NULL, NULL },

	};

	luaL_newlibtable(L, l);

	lua_getfield(L, LUA_REGISTRYINDEX, "Context");

	tengine::Context *context = (tengine::Context*)lua_touserdata(L, -1);

	if (context == NULL)
	{
		return luaL_error(L, "please init Context...");
	}

	lua_getfield(L, LUA_REGISTRYINDEX, "SandBox");

	luaL_setfuncs(L, l, 2);

	return 1;
}


namespace tengine
{
	SandBox::SandBox(Context& context)
		: SandBox(context, "")
	{

	}

	SandBox::SandBox(Context& context, const char *args)
		: Service(context)
		, l_(nullptr)
		, args_(args)
		, timer_(nullptr)
		, logger_(nullptr)
		, channels()
		, udp_channels()
	{

	}

	SandBox::~SandBox()
	{
		channels.clear();

		udp_channels.clear();

		if (l_)
		{
			lua_close(l_);
		}
	}


	lua_State* SandBox::state()
	{
		return l_;
	}

	int SandBox::init(const char* name)
	{
		Service::init(name);

		lua_State *L = lua_newstate(cclalloc, NULL);
		l_ = L;

		if (!L)
			return -1;

		asio::post(executor(),
			[=]
			{
				const std::string& args = this->args_;
				this->inject(name, args.c_str(), args.size());
			});

		return 0;
	}

	int SandBox::inject(const char *name, const char* args, std::size_t sz)
	{
		lua_State * L = l_;

		luaL_openlibs(L);

#ifdef LUA_JIT
		luaL_requiref(L, "compat53.string", luaopen_string, 0);
		luaL_requiref(L, "compat53.table", luaopen_table, 0);
		luaL_requiref(L, "compat53.utf8", luaopen_utf8, 0);
#endif

		lua_pushlightuserdata(L, &context_);
		lua_setfield(L, LUA_REGISTRYINDEX, "Context");

		lua_pushlightuserdata(L, this);
		lua_setfield(L, LUA_REGISTRYINDEX, "SandBox");

		lua_pushinteger(L, this->id());
		lua_setglobal(L, "__Service__");

		luaL_requiref(L, "tengine.c", luaopen_tengine, 0);
		lua_pop(L, 1);

		const char* path = context_.config("lua_path", "./scripts");

		lua_pushstring(L, path);
		lua_setglobal(L, "LUA_PATH");

		char load_path[512];

		sprintf(load_path, "package.path=package.path..';%s/?.lua;%s/?.luac'.. ';?/init.lua;?/init.luac' .. ';%s/%s/?.lua;%s/%s/?.luac'",
			path, path, path, name, path, name);

		if (luaL_dostring(L, load_path) != 0)
			return -1;

		const char* c_path = context_.config("c_path", "");
		sprintf(load_path, "package.cpath=package.cpath..';%s'", c_path);	

		if (luaL_dostring(L, load_path) != 0)
			return -1;

		static const char * scripts = "\
			local tmp = {...}\n \
			local name = tmp[1]\n \
			local args = {}\n \
			for word in string.gmatch(tmp[2], '%S+') do\n \
				table.insert(args, word)\n \
			end\n \
			local errs = {}\n \
			local main, pattern\n \
			for p in string.gmatch(string.format('./%s/?.lua', name), '([^;]+);*') do\n \
				local filename = string.gsub(p, '?', 'main')\n \
				local f, msg = loadfile(filename)\n \
				if not f then\n \
					table.insert(errs, msg)\n \
				else\n \
					pattern = p\n \
					main = f\n \
					break\n \
				end\n \
			end\n \
			if not main then\n \
				for p in string.gmatch(string.format('./tengine/%s/init.lua', name), '([^;]+);*') do\n \
					local filename = string.gsub(p, '?', name)\n \
					local f, msg = loadfile(filename)\n \
					if not f then\n \
						table.insert(errs, msg)\n \
					else\n \
						pattern = p\n \
						main = f\n \
						break\n \
					end\n \
				end\n \
			end\n \
			if not main then\n \
				error(table.concat(errs, ' '))\n \
			end\n \
			main(table.unpack(args))\n \
			";	

		int ret = luaL_loadstring(L, scripts);
		if (ret != LUA_OK)
		{
			Logger *logger = (Logger*)context_.query("Logger");
			if (logger != nullptr)
			{
				logger->log(lua_tostring(L, -1), this);
			}

			return -1;
		}
		lua_pushstring(L, name);

		lua_pushlstring(L, args, sz);

		ret = lua_pcall(L, 2, 0, 0);
		if (ret != LUA_OK)
		{
			Logger *logger = (Logger*)context_.query("Logger");
			if (logger != nullptr)
			{
				logger->log(lua_tostring(L, -1), this);
			}

			return -1;
		}
		
		lua_settop(L,0);

		return 0;
	}

	int SandBox::call(int num, bool remove, lua_State* L)
	{
		if (!L)
			L = l_;

		int func_index = -(num + 1);
		if (!lua_isfunction(L, func_index))
		{
			lua_pop(L, func_index + 1);
			return luaL_error(L, "value at stack [%d] is not function !!!", func_index);;
		}

		int traceback = 0;
		lua_getglobal(L, "__G__TRACKBACK__");
		if (!lua_isfunction(L, -1))
		{
			lua_pop(L, 1);
		}
		else
		{
			lua_insert(L, func_index - 1);
			traceback = func_index - 1;
		}

		int error = 0;
		error = lua_pcall(L, num, 1, traceback);
		if (error)
		{
			if (traceback == 0)
			{
				Logger *logger = (Logger*)context_.query("Logger");
				if (logger != NULL)
				{
					logger->log(lua_tostring(L, -1), this);
				}

				lua_pop(L, 1);
			}
			else
			{
				lua_pop(L, 2);
			}

			return 0;
		}

		int ret = 0;
		if (remove)
		{
			if (lua_isnumber(L, -1))
			{
				ret = (int)lua_tointeger(L, -1);
			}
			else if (lua_isboolean(L, -1))
			{
				ret = lua_toboolean(L, -1);
			}

			lua_pop(L, 1);
		}
		else
		{
			ret = 1;
		}

		if (traceback)
		{
			lua_remove(L, remove ? -1 : -2);
		}

		return ret;
	}

	void SandBox::handler(int from, const fs::path &path, void* data)
	{
		lua_State *L = l_;

		lua_rawgetp(L, LUA_REGISTRYINDEX, &WatchDog::WATCHDOG_KEY);

		lua_rawgetp(L, -1, data);

		struct watchdog* w = (struct watchdog*)lua_touserdata(L, -1);
		if (w != NULL)
		{
			lua_rawgeti(L, LUA_REGISTRYINDEX, w->callback);
			lua_pushlstring(L, path.string().c_str(), path.string().size());
			call(1, true);
		}
	}

	void SandBox::handler(int from, const std::vector<fs::path> &paths, void* data)
	{
		lua_State *L = l_;

		lua_rawgetp(L, LUA_REGISTRYINDEX, &WatchDog::WATCHDOG_KEY);

		lua_rawgetp(L, -1, data);

		struct watchdog* w = (struct watchdog*)lua_touserdata(L, -1);
		if (w != NULL)
		{
			lua_rawgeti(L, LUA_REGISTRYINDEX, w->callback);
			lua_newtable(L);

			for (std::size_t i = 0; i < paths.size(); i++)
			{
				lua_pushinteger(L, i + 1);
				lua_pushlstring(L, paths[i].string().c_str(), paths[i].string().size());
				lua_settable(L, -3);
			}


			call(1, true);
		}
	}
}
