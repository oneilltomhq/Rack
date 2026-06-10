#include <agent.hpp>

#include <condition_variable>
#include <cstdio>
#include <deque>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include <jansson.h>

#include <app/CableWidget.hpp>
#include <app/ModuleWidget.hpp>
#include <app/RackWidget.hpp>
#include <app/Scene.hpp>
#include <context.hpp>
#include <engine/Cable.hpp>
#include <engine/Engine.hpp>
#include <engine/Module.hpp>
#include <logger.hpp>
#include <patch.hpp>
#include <plugin.hpp>
#include <settings.hpp>


namespace rack {
namespace agent {


namespace {


struct Request {
	json_t* inputJ = NULL;
	json_t* responseJ = NULL;
	bool done = false;
};

std::mutex mutex;
std::condition_variable cv;
std::deque<Request*> requests;
std::thread inputThread;
bool running = false;
bool stopping = false;
std::map<std::string, int64_t> moduleAliases;


json_t* makeError(json_t* idJ, int code, const char* message) {
	json_t* responseJ = json_object();
	json_object_set_new(responseJ, "jsonrpc", json_string("2.0"));
	if (idJ)
		json_object_set(responseJ, "id", idJ);
	else
		json_object_set_new(responseJ, "id", json_null());

	json_t* errorJ = json_object();
	json_object_set_new(errorJ, "code", json_integer(code));
	json_object_set_new(errorJ, "message", json_string(message));
	json_object_set_new(responseJ, "error", errorJ);
	return responseJ;
}


json_t* makeResult(json_t* idJ, json_t* resultJ) {
	json_t* responseJ = json_object();
	json_object_set_new(responseJ, "jsonrpc", json_string("2.0"));
	if (idJ)
		json_object_set(responseJ, "id", idJ);
	else
		json_object_set_new(responseJ, "id", json_null());
	json_object_set_new(responseJ, "result", resultJ);
	return responseJ;
}


json_t* getParams(json_t* requestJ) {
	json_t* paramsJ = json_object_get(requestJ, "params");
	if (paramsJ && json_is_object(paramsJ))
		return paramsJ;
	return NULL;
}


const char* getString(json_t* objectJ, const char* key) {
	json_t* valueJ = json_object_get(objectJ, key);
	if (!json_is_string(valueJ))
		return NULL;
	return json_string_value(valueJ);
}


bool getInt(json_t* objectJ, const char* key, int* value) {
	json_t* valueJ = json_object_get(objectJ, key);
	if (!json_is_integer(valueJ))
		return false;
	*value = (int) json_integer_value(valueJ);
	return true;
}


bool getFloat(json_t* objectJ, const char* key, float* value) {
	json_t* valueJ = json_object_get(objectJ, key);
	if (!json_is_number(valueJ))
		return false;
	*value = (float) json_number_value(valueJ);
	return true;
}


bool getModuleId(json_t* objectJ, const char* key, int64_t* moduleId) {
	json_t* moduleJ = json_object_get(objectJ, key);
	if (json_is_integer(moduleJ)) {
		*moduleId = json_integer_value(moduleJ);
		return true;
	}
	if (json_is_string(moduleJ)) {
		auto it = moduleAliases.find(json_string_value(moduleJ));
		if (it == moduleAliases.end())
			return false;
		*moduleId = it->second;
		return true;
	}
	return false;
}


bool getModuleId(json_t* objectJ, int64_t* moduleId) {
	return getModuleId(objectJ, "module", moduleId);
}


json_t* clearPatch(json_t* idJ) {
	APP->patch->clear();
	moduleAliases.clear();
	json_t* resultJ = json_object();
	json_object_set_new(resultJ, "cleared", json_boolean(true));
	return makeResult(idJ, resultJ);
}


json_t* listModels(json_t* idJ, json_t* paramsJ) {
	const char* filterPluginSlug = paramsJ ? getString(paramsJ, "plugin") : NULL;

	json_t* resultJ = json_object();
	json_object_set_new(resultJ, "plugins", json_array());
	json_t* pluginsJ = json_object_get(resultJ, "plugins");

	for (plugin::Plugin* p : plugin::plugins) {
		if (filterPluginSlug && p->slug != filterPluginSlug)
			continue;

		json_t* pluginJ = json_object();
		json_object_set_new(pluginJ, "slug", json_string(p->slug.c_str()));
		json_object_set_new(pluginJ, "name", json_string(p->name.c_str()));
		json_object_set_new(pluginJ, "brand", json_string(p->getBrand().c_str()));
		json_object_set_new(pluginJ, "version", json_string(p->version.c_str()));
		json_object_set_new(pluginJ, "models", json_array());

		json_t* modelsJ = json_object_get(pluginJ, "models");
		for (plugin::Model* model : p->models) {
			json_t* modelJ = json_object();
			json_object_set_new(modelJ, "slug", json_string(model->slug.c_str()));
			json_object_set_new(modelJ, "name", json_string(model->name.c_str()));
			json_object_set_new(modelJ, "hidden", json_boolean(model->hidden));
			json_array_append_new(modelsJ, modelJ);
		}

		json_object_set_new(pluginJ, "modelCount", json_integer((json_int_t) json_array_size(modelsJ)));
		json_array_append_new(pluginsJ, pluginJ);
	}

	json_object_set_new(resultJ, "pluginCount", json_integer((json_int_t) json_array_size(pluginsJ)));
	return makeResult(idJ, resultJ);
}


json_t* createModule(json_t* idJ, json_t* paramsJ) {
	if (!paramsJ)
		return makeError(idJ, -32602, "Missing params object");

	const char* pluginSlug = getString(paramsJ, "plugin");
	const char* modelSlug = getString(paramsJ, "model");
	if (!pluginSlug || !modelSlug)
		return makeError(idJ, -32602, "create_module requires plugin and model");

	plugin::Model* model = plugin::getModel(pluginSlug, modelSlug);
	if (!model)
		return makeError(idJ, -32000, "Model not found");

	engine::Module* module = model->createModule();
	if (!module)
		return makeError(idJ, -32000, "Model did not create a module");

	app::ModuleWidget* moduleWidget = NULL;
	if (!settings::headless) {
		moduleWidget = model->createModuleWidget(module);
		if (!moduleWidget) {
			delete module;
			return makeError(idJ, -32000, "Model did not create a module widget");
		}
	}

	APP->engine->addModule(module);

	if (moduleWidget) {
		double x = 0.0;
		double y = 0.0;
		json_t* posJ = json_object_get(paramsJ, "pos");
		if (posJ)
			json_unpack(posJ, "[F, F]", &x, &y);
		moduleWidget->setGridPosition(math::Vec(x, y));
		APP->scene->rack->addModule(moduleWidget);
		APP->scene->rack->updateExpanders();
	}

	const char* alias = getString(paramsJ, "id");
	if (alias)
		moduleAliases[alias] = module->id;

	json_t* resultJ = json_object();
	json_object_set_new(resultJ, "moduleId", json_integer(module->id));
	if (alias)
		json_object_set_new(resultJ, "id", json_string(alias));
	json_object_set_new(resultJ, "plugin", json_string(pluginSlug));
	json_object_set_new(resultJ, "model", json_string(modelSlug));
	return makeResult(idJ, resultJ);
}


json_t* setParam(json_t* idJ, json_t* paramsJ) {
	if (!paramsJ)
		return makeError(idJ, -32602, "Missing params object");

	int64_t moduleId = -1;
	int paramId = -1;
	float value = 0.f;
	if (!getModuleId(paramsJ, &moduleId) || !getInt(paramsJ, "param", &paramId) || !getFloat(paramsJ, "value", &value))
		return makeError(idJ, -32602, "set_param requires module, param, and value");

	engine::Module* module = APP->engine->getModule(moduleId);
	if (!module)
		return makeError(idJ, -32000, "Module not found");
	if (paramId < 0 || paramId >= (int) module->params.size())
		return makeError(idJ, -32602, "Param index out of range");

	APP->engine->setParamValue(module, paramId, value);

	json_t* resultJ = json_object();
	json_object_set_new(resultJ, "moduleId", json_integer(moduleId));
	json_object_set_new(resultJ, "param", json_integer(paramId));
	json_object_set_new(resultJ, "value", json_real(value));
	return makeResult(idJ, resultJ);
}


json_t* connect(json_t* idJ, json_t* paramsJ) {
	if (!paramsJ)
		return makeError(idJ, -32602, "Missing params object");

	int64_t outputModuleId = -1;
	int64_t inputModuleId = -1;
	int outputId = -1;
	int inputId = -1;
	if (!getModuleId(paramsJ, "outputModule", &outputModuleId)
		|| !getModuleId(paramsJ, "inputModule", &inputModuleId)
		|| !getInt(paramsJ, "output", &outputId)
		|| !getInt(paramsJ, "input", &inputId)) {
		return makeError(idJ, -32602, "connect requires outputModule, output, inputModule, and input");
	}

	engine::Module* outputModule = APP->engine->getModule(outputModuleId);
	engine::Module* inputModule = APP->engine->getModule(inputModuleId);
	if (!outputModule || !inputModule)
		return makeError(idJ, -32000, "Module not found");
	if (outputId < 0 || outputId >= (int) outputModule->outputs.size())
		return makeError(idJ, -32602, "Output index out of range");
	if (inputId < 0 || inputId >= (int) inputModule->inputs.size())
		return makeError(idJ, -32602, "Input index out of range");

	engine::Cable* cable = new engine::Cable;
	cable->outputModule = outputModule;
	cable->outputId = outputId;
	cable->inputModule = inputModule;
	cable->inputId = inputId;
	APP->engine->addCable(cable);

	if (!settings::headless) {
		app::CableWidget* cableWidget = new app::CableWidget;
		try {
			cableWidget->setCable(cable);
			cableWidget->color = APP->scene->rack->getNextCableColor();
			APP->scene->rack->addCable(cableWidget);
		}
		catch (Exception& e) {
			delete cableWidget;
			APP->engine->removeCable(cable);
			delete cable;
			return makeError(idJ, -32000, e.what());
		}
	}

	json_t* resultJ = json_object();
	json_object_set_new(resultJ, "cableId", json_integer(cable->id));
	json_object_set_new(resultJ, "outputModuleId", json_integer(outputModuleId));
	json_object_set_new(resultJ, "output", json_integer(outputId));
	json_object_set_new(resultJ, "inputModuleId", json_integer(inputModuleId));
	json_object_set_new(resultJ, "input", json_integer(inputId));
	return makeResult(idJ, resultJ);
}


json_t* configureAudio(json_t* idJ, json_t* paramsJ) {
	if (!paramsJ)
		return makeError(idJ, -32602, "Missing params object");

	int64_t moduleId = -1;
	if (!getModuleId(paramsJ, &moduleId))
		return makeError(idJ, -32602, "configure_audio requires module");

	engine::Module* module = APP->engine->getModule(moduleId);
	if (!module)
		return makeError(idJ, -32000, "Module not found");

	int driver = -1;
	int blockSize = 256;
	float sampleRate = 48000.f;
	getInt(paramsJ, "driver", &driver);
	getInt(paramsJ, "blockSize", &blockSize);
	getFloat(paramsJ, "sampleRate", &sampleRate);
	const char* deviceName = getString(paramsJ, "deviceName");

	json_t* dataJ = json_object();
	json_t* audioJ = json_object();
	json_object_set_new(audioJ, "driver", json_integer(driver));
	if (deviceName)
		json_object_set_new(audioJ, "deviceName", json_string(deviceName));
	json_object_set_new(audioJ, "sampleRate", json_real(sampleRate));
	json_object_set_new(audioJ, "blockSize", json_integer(blockSize));
	json_object_set_new(dataJ, "audio", audioJ);
	module->dataFromJson(dataJ);
	json_decref(dataJ);

	json_t* resultJ = json_object();
	json_object_set_new(resultJ, "moduleId", json_integer(moduleId));
	json_object_set_new(resultJ, "configured", json_boolean(true));
	return makeResult(idJ, resultJ);
}


json_t* listModules(json_t* idJ) {
	json_t* resultJ = json_object();
	json_object_set_new(resultJ, "modules", json_array());
	json_t* modulesJ = json_object_get(resultJ, "modules");

	for (int64_t moduleId : APP->engine->getModuleIds()) {
		engine::Module* module = APP->engine->getModule(moduleId);
		if (!module)
			continue;

		json_t* moduleJ = json_object();
		json_object_set_new(moduleJ, "moduleId", json_integer(moduleId));
		if (module->model) {
			if (module->model->plugin)
				json_object_set_new(moduleJ, "plugin", json_string(module->model->plugin->slug.c_str()));
			json_object_set_new(moduleJ, "model", json_string(module->model->slug.c_str()));
			json_object_set_new(moduleJ, "name", json_string(module->model->name.c_str()));
		}
		json_array_append_new(modulesJ, moduleJ);
	}

	json_object_set_new(resultJ, "moduleCount", json_integer((json_int_t) json_array_size(modulesJ)));
	return makeResult(idJ, resultJ);
}


json_t* handleRequest(json_t* requestJ) {
	json_t* idJ = json_object_get(requestJ, "id");
	json_t* methodJ = json_object_get(requestJ, "method");
	if (!json_is_string(methodJ))
		return makeError(idJ, -32600, "Missing JSON-RPC method");

	const char* method = json_string_value(methodJ);
	json_t* paramsJ = getParams(requestJ);

	if (std::string(method) == "list_models")
		return listModels(idJ, paramsJ);
	if (std::string(method) == "clear")
		return clearPatch(idJ);
	if (std::string(method) == "create_module")
		return createModule(idJ, paramsJ);
	if (std::string(method) == "set_param")
		return setParam(idJ, paramsJ);
	if (std::string(method) == "connect")
		return connect(idJ, paramsJ);
	if (std::string(method) == "configure_audio")
		return configureAudio(idJ, paramsJ);
	if (std::string(method) == "list_modules")
		return listModules(idJ);

	return makeError(idJ, -32601, "Method not found");
}


void writeJson(json_t* rootJ) {
	json_dumpf(rootJ, stdout, JSON_COMPACT);
	std::fprintf(stdout, "\n");
	std::fflush(stdout);
}


void inputMain() {
	std::string line;

	while (!stopping && std::getline(std::cin, line)) {
		json_error_t error;
		json_t* inputJ = json_loads(line.c_str(), 0, &error);
		if (!inputJ) {
			json_t* responseJ = makeError(NULL, -32700, error.text);
			writeJson(responseJ);
			json_decref(responseJ);
			continue;
		}

		Request request;
		request.inputJ = inputJ;

		{
			std::lock_guard<std::mutex> lock(mutex);
			requests.push_back(&request);
		}
		cv.notify_all();

		std::unique_lock<std::mutex> lock(mutex);
		cv.wait(lock, [&] {
			return request.done || stopping;
		});

		if (request.responseJ) {
			writeJson(request.responseJ);
			json_decref(request.responseJ);
		}
		json_decref(inputJ);
	}

	{
		std::lock_guard<std::mutex> lock(mutex);
		stopping = true;
		running = false;
	}
	cv.notify_all();
}


} // namespace


void startStdio() {
	std::lock_guard<std::mutex> lock(mutex);
	if (running)
		return;
	stopping = false;
	running = true;
	inputThread = std::thread(inputMain);
}


void process() {
	while (true) {
		Request* request = NULL;
		{
			std::lock_guard<std::mutex> lock(mutex);
			if (requests.empty())
				return;
			request = requests.front();
			requests.pop_front();
		}

		json_t* responseJ = handleRequest(request->inputJ);

		{
			std::lock_guard<std::mutex> lock(mutex);
			request->responseJ = responseJ;
			request->done = true;
		}
		cv.notify_all();
	}
}


void stop() {
	{
		std::lock_guard<std::mutex> lock(mutex);
		stopping = true;
		running = false;
	}
	cv.notify_all();

	if (inputThread.joinable())
		inputThread.detach();

	while (!requests.empty()) {
		Request* request = requests.front();
		requests.pop_front();
		request->done = true;
	}
}


bool isRunning() {
	std::lock_guard<std::mutex> lock(mutex);
	return running;
}


} // namespace agent
} // namespace rack
