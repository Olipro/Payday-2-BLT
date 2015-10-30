#include "InitState.h"

#include "signatures/signatures.h"
#include "util/util.h"
#include "console/console.h"
#include "threading/queue.h"
#include "http/http.h"

#include <thread>
#include <list>

#define LUA_REGISTRYINDEX	(-10000)
#define LUA_GLOBALSINDEX	(-10002)

std::string moduleFile;

typedef void lua_State;
typedef const char* (*lua_Reader) (lua_State *L, void *data, size_t *size);
typedef LPVOID (*lua_Alloc)(LPVOID, LPVOID, size_t, size_t);
typedef int(*lua_CFunction) (lua_State *L);
typedef struct luaL_Reg {
	const char* name;
	lua_CFunction func;
} luaL_Reg;

enum HookCallbackType
{
	NEWSTATE_CALLBACK,
	GAMETICK_CALLBACK,
	REQUIRE_CALLBACK,
	CLOSESTATE_CALLBACK,

	CALLBACK_ENUM_END,
};

typedef void(WINAPI* CallbackFunction)(lua_State*, LPCSTR, LPVOID);

void(*RegisterCallback)(const HookCallbackType, CallbackFunction, LPVOID);

LUAFUNC_PTR(lua_call, void, lua_State*, int, int);
LUAFUNC_PTR(lua_pcall, int, lua_State*, int, int, int);
LUAFUNC_PTR(lua_gettop, int, lua_State*);
LUAFUNC_PTR(lua_settop, void, lua_State*, int);
LUAFUNC_PTR(lua_tolstring, const char*, lua_State*, int, size_t*);
LUAFUNC_PTR(luaL_loadfile, int, lua_State*, const char*);
LUAFUNC_PTR(lua_load, int, lua_State*, lua_Reader, void*, const char*);
LUAFUNC_PTR(lua_setfield, void, lua_State*, int, const char*);
LUAFUNC_PTR(lua_createtable, void, lua_State*, int, int);
LUAFUNC_PTR(lua_insert, void, lua_State*, int);
LUAFUNC_PTR(lua_newstate, lua_State*, lua_Alloc, void*);
LUAFUNC_PTR(lua_close, void, lua_State*);
LUAFUNC_PTR(lua_rawset, void, lua_State*, int);
LUAFUNC_PTR(lua_settable, void, lua_State*, int);
LUAFUNC_PTR(lua_pushnumber, void, lua_State*, double);
LUAFUNC_PTR(lua_pushinteger, void, lua_State*, unsigned int);
LUAFUNC_PTR(lua_pushboolean, void, lua_State*, bool);
LUAFUNC_PTR(lua_pushcclosure, void, lua_State*, lua_CFunction, int);
LUAFUNC_PTR(lua_pushlstring, void, lua_State*, const char*, size_t);
LUAFUNC_PTR(luaL_openlib, void, lua_State*, const char*, const luaL_Reg*, int);
LUAFUNC_PTR(luaL_ref, int, lua_State*, int);
LUAFUNC_PTR(lua_rawgeti, void, lua_State*, int, int);
LUAFUNC_PTR(luaL_unref, void, lua_State*, int, int);
HOOK_PTR(do_game_update, __thiscall, void*, void*, int*, int*);
HOOK_PTR(luaL_newstate, __thiscall, int, void*, char, char, int);

// more bloody lua shit
#define LUA_YIELD	1
#define LUA_ERRRUN	2
#define LUA_ERRSYNTAX	3
#define LUA_ERRMEM	4
#define LUA_ERRERR	5
#define LUA_ERRFILE     (LUA_ERRERR+1)

std::list<lua_State*> activeStates;
void add_active_state(lua_State* L){
	activeStates.push_back(L);
}

void remove_active_state(lua_State* L){
	activeStates.remove(L);
}

bool check_active_state(lua_State* L){
	std::list<lua_State*>::iterator it;
	for (it = activeStates.begin(); it != activeStates.end(); it++){
		if (*it == L) {
			return true;
		}
	}
	return false;
}

int luaH_getcontents(lua_State* L, bool files){
	size_t len;
	const char* dirc = lua_tolstring(L, 1, &len);
	std::string dir(dirc, len);
	std::vector<std::string> directories;

	try {
		directories = Util::GetDirectoryContents(dir, files);
	}
	catch (...){
		lua_pushboolean(L, false);
		return 1;
	}

	lua_createtable(L, 0, 0);

	std::vector<std::string>::iterator it;
	int index = 1;
	for (it = directories.begin(); it < directories.end(); it++){
		if (*it == "." || *it == "..") continue;
		lua_pushinteger(L, index);
		lua_pushlstring(L, it->c_str(), it->length());
		lua_settable(L, -3);
		index++;
	}

	return 1;
}

int luaF_getdir(lua_State* L){
	return luaH_getcontents(L, true);
}

int luaF_getfiles(lua_State* L){
	return luaH_getcontents(L, false);
}

int luaF_directoryExists(lua_State* L){
	size_t len;
	const char* dirc = lua_tolstring(L, 1, &len);
	bool doesExist = Util::DirectoryExists(dirc);
	lua_pushboolean(L, doesExist);
	return 1;
}

int luaF_unzipfile(lua_State* L){
	size_t len;
	const char* archivePath = lua_tolstring(L, 1, &len);
	const char* extractPath = lua_tolstring(L, 2, &len);

	ZIPArchive* archive = new ZIPArchive(archivePath, extractPath);
	archive->ReadArchive();
	delete archive;
	return 0;
}

int luaF_removeDirectory(lua_State* L){
	size_t len;
	const char* directory = lua_tolstring(L, 1, &len);
	bool success = Util::RemoveEmptyDirectory(directory);
	lua_pushboolean(L, success);
	return 1;
}

int luaF_pcall(lua_State* L){
	int args = lua_gettop(L);

	int result = lua_pcall(L, args - 1, -1, 0);
	if (result == LUA_ERRRUN){
		size_t len;
		Logging::Log(lua_tolstring(L, -1, &len), Logging::LOGGING_ERROR);
		return 0;
	}
	lua_pushboolean(L, result == 0);
	lua_insert(L, 1);

	//if (result != 0) return 1;

	return lua_gettop(L);
}

int luaF_dofile(lua_State* L){

	int n = lua_gettop(L);

	size_t length = 0;
	const char* filename = lua_tolstring(L, 1, &length);
	int error = luaL_loadfile(L, filename);
	if (error == LUA_ERRSYNTAX){
		size_t len;
		Logging::Log(filename, Logging::LOGGING_ERROR);
		Logging::Log(lua_tolstring(L, -1, &len), Logging::LOGGING_ERROR);
	}
	error = lua_pcall(L, 0, 0, 0);
	if (error == LUA_ERRRUN){
		size_t len;
		Logging::Log(filename, Logging::LOGGING_ERROR);
		Logging::Log(lua_tolstring(L, -1, &len), Logging::LOGGING_ERROR);
	}
	return 0;
}

struct lua_http_data {
	int funcRef;
	int progressRef;
	int requestIdentifier;
	lua_State* L;
};

void return_lua_http(void* data, std::string& urlcontents){
	lua_http_data* ourData = (lua_http_data*)data;

	if (!check_active_state(ourData->L)) {
		delete ourData;
		return;
	}

	lua_rawgeti(ourData->L, LUA_REGISTRYINDEX, ourData->funcRef);
	lua_pushlstring(ourData->L, urlcontents.c_str(), urlcontents.length());
	lua_pushinteger(ourData->L, ourData->requestIdentifier);
	lua_pcall(ourData->L, 2, 0, 0);
	luaL_unref(ourData->L, LUA_REGISTRYINDEX, ourData->funcRef);
	luaL_unref(ourData->L, LUA_REGISTRYINDEX, ourData->progressRef);
	delete ourData;
}

void progress_lua_http(void* data, long progress, long total){
	lua_http_data* ourData = (lua_http_data*)data;

	if (!check_active_state(ourData->L)){
		return;
	}

	if (ourData->progressRef == 0) return;
	lua_rawgeti(ourData->L, LUA_REGISTRYINDEX, ourData->progressRef);
	lua_pushinteger(ourData->L, ourData->requestIdentifier);
	lua_pushinteger(ourData->L, progress);
	lua_pushinteger(ourData->L, total);
	lua_pcall(ourData->L, 3, 0, 0);
}

static int HTTPReqIdent = 0;

int luaF_dohttpreq(lua_State* L){
	Logging::Log("Incoming HTTP Request/Request");

	int args = lua_gettop(L);
	int progressReference = 0;
	if (args >= 3){
		progressReference = luaL_ref(L, LUA_REGISTRYINDEX);
	}

	int functionReference = luaL_ref(L, LUA_REGISTRYINDEX);
	size_t len;
	const char* url_c = lua_tolstring(L, 1, &len);
	std::string url = std::string(url_c, len);

	Logging::Log(url);
	Logging::Log(std::to_string(functionReference));

	lua_http_data* ourData = new lua_http_data();
	ourData->funcRef = functionReference;
	ourData->progressRef = progressReference;
	ourData->L = L;

	HTTPReqIdent++;
	ourData->requestIdentifier = HTTPReqIdent;

	HTTPItem* reqItem = new HTTPItem();
	reqItem->call = return_lua_http;
	reqItem->data = ourData;
	reqItem->url = url;

	if (progressReference != 0){
		reqItem->progress = progress_lua_http;
	}

	HTTPManager::GetSingleton().LaunchHTTPRequest(reqItem);
	lua_pushinteger(L, HTTPReqIdent);
	return 1;
}

CConsole* gbl_mConsole = NULL;

int luaF_createconsole(lua_State* L){
	if (gbl_mConsole) return 0;
	gbl_mConsole = new CConsole();
	return 0;
}

int luaF_destroyconsole(lua_State* L){
	if (!gbl_mConsole) return 0;
	delete gbl_mConsole;
	gbl_mConsole = NULL;
	return 0;
}

int luaF_print(lua_State* L){
	size_t len;
	const char* str = lua_tolstring(L, 1, &len);
	Logging::Log(str, Logging::LOGGING_LUA);
	return 0;
}

int updates = 0;
std::thread::id main_thread_id;

void* __fastcall do_game_update_new(void* thislol, int edx, int* a, int* b){

	// If someone has a better way of doing this, I'd like to know about it.
	// I could save the this pointer?
	// I'll check if it's even different at all later.
	lua_State* L = thislol;

	if (updates == 0){
		HTTPManager::GetSingleton().init_locks();
	}

	if (updates > 1){
		EventQueueM::GetSingleton().ProcessEvents();
	}

	updates++;
	return nullptr;
}

// Random dude who wrote what's his face?
// I 'unno, I stole this method from the guy who wrote the 'underground-light-lua-hook'
// Mine worked fine, but this seems more elegant.

//If you want elegant, make an actual class member function and recast it through vararg abuse. -Olipro.
EXTERN_C IMAGE_DOS_HEADER __ImageBase;
int __fastcall luaL_newstate_new(void* thislol, int edx, char no, char freakin, int clue){
	lua_State *L = nullptr;
	int ret = 0, stack_size = 0;
	L = thislol;

	add_active_state(L);
	CREATE_LUA_FUNCTION(luaF_dohttpreq, "dohttpreq")
	CREATE_LUA_FUNCTION(luaF_print, "log")
	CREATE_LUA_FUNCTION(luaF_unzipfile, "unzip")

	luaL_Reg consoleLib[] = { { "CreateConsole", luaF_createconsole }, { "DestroyConsole", luaF_destroyconsole }, { NULL, NULL } };
	luaL_openlib(L, "console", consoleLib, 0);

	luaL_Reg fileLib[] = { { "GetDirectories", luaF_getdir }, { "GetFiles", luaF_getfiles }, { "RemoveDirectory", luaF_removeDirectory }, { "DirectoryExists", luaF_directoryExists }, { NULL, NULL } };
	luaL_openlib(L, "file", fileLib, 0);

	static std::string dir, file, safe_path;
	if (!dir.length())
	{
		TCHAR buf[MAX_PATH];
		std::copy(moduleFile.begin(), moduleFile.end(), buf);
		PathRemoveFileSpec(buf);
		dir = safe_path = buf;
		std::copy(moduleFile.begin(), moduleFile.end(), buf);
		PathStripPath(buf);
		file = buf;
		safe_path += "/../";
	}
	lua_createtable(L, 2, 0);
	lua_pushlstring(L, moduleFile.c_str(), moduleFile.length());
	lua_setfield(L, -2, "hook_dll_path");
	lua_pushlstring(L, dir.c_str(), dir.length());
	lua_setfield(L, -2, "hook_dll_dir");
	lua_pushlstring(L, file.c_str(), file.length());
	lua_setfield(L, -2, "hook_dll_file");
	lua_pushlstring(L, safe_path.c_str(), safe_path.length());
	lua_setfield(L, -2, "hook_dll_safe_path");

	lua_setfield(L, LUA_GLOBALSINDEX, "BLTDLLInfo");

	int result;
	Logging::Log("Initiating Hook");
	
	result = luaL_loadfile(L, "mods/base/base.lua");
	if (result == LUA_ERRSYNTAX){
		size_t len;
		Logging::Log(lua_tolstring(L, -1, &len), Logging::LOGGING_ERROR);
		return ret;
	}
	result = lua_pcall(L, 0, 1, 0);
	if (result == LUA_ERRRUN){
		size_t len;
		Logging::Log(lua_tolstring(L, -1, &len), Logging::LOGGING_ERROR);
		return ret;
	}

	return ret;
}

void luaF_close(lua_State* L){
	remove_active_state(L);
}

void GetSignatures(HMODULE hDLL) {

	//RESOLVE(hDLL, lua_call);
	RESOLVE(hDLL, lua_pcall);
	RESOLVE(hDLL, lua_gettop);
	RESOLVE(hDLL, lua_settop);
	RESOLVE(hDLL, lua_tolstring);
	RESOLVE(hDLL, luaL_loadfile);
	RESOLVE(hDLL, lua_load);
	RESOLVE(hDLL, lua_setfield);
	RESOLVE(hDLL, lua_createtable);
	RESOLVE(hDLL, lua_insert);
	RESOLVE(hDLL, lua_newstate);
	RESOLVE(hDLL, lua_rawset);
	RESOLVE(hDLL, lua_settable);
	RESOLVE(hDLL, lua_pushnumber);
	RESOLVE(hDLL, lua_pushinteger);
	RESOLVE(hDLL, lua_pushboolean);
	RESOLVE(hDLL, lua_pushcclosure);
	RESOLVE(hDLL, lua_pushlstring);
	RESOLVE(hDLL, luaL_openlib);
	RESOLVE(hDLL, luaL_ref);
	RESOLVE(hDLL, lua_rawgeti);
	RESOLVE(hDLL, luaL_unref);
	RESOLVE(hDLL, RegisterCallback);
}

static HTTPManager& mainManager = HTTPManager::GetSingleton();

void InitiateStates(){

	main_thread_id = std::this_thread::get_id();

	GetSignatures(GetModuleHandle("IPHLPAPI.dll"));
	RegisterCallback(GAMETICK_CALLBACK, [](lua_State* L, LPCSTR, LPVOID) { do_game_update_new(L, 0, nullptr, nullptr); }, nullptr);
	RegisterCallback(NEWSTATE_CALLBACK, [](lua_State* L, LPCSTR, LPVOID) { luaL_newstate_new(L, 0, 0, 0, 0); }, nullptr);
	RegisterCallback(CLOSESTATE_CALLBACK, [](lua_State* L, LPCSTR, LPVOID) { luaF_close(L); }, nullptr);
	
	new EventQueueM();
}

void DestroyStates(){
	// Okay... let's not do that.
	// I don't want to keep this in memory, but it CRASHES THE SHIT OUT if you delete this after all is said and done.
	// if (gbl_mConsole) delete gbl_mConsole;
}
