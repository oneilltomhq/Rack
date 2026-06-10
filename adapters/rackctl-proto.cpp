#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <asset.hpp>
#include <context.hpp>
#include <engine/Engine.hpp>
#include <engine/Module.hpp>
#include <logger.hpp>
#include <plugin.hpp>
#include <random.hpp>
#include <settings.hpp>
#include <string.hpp>
#include <system.hpp>

#include <jansson.h>

using namespace rack;

namespace {

struct Options {
	std::string systemDir = ".";
	std::string userDir = "agent-user";
	std::string pluginSlug;
	int frames = 48000;
	float pitch = 0.f;
};

void usage(const char* argv0) {
	std::fprintf(stderr,
		"Usage:\n"
		"  %s list-models [--system DIR] [--user DIR] [--plugin SLUG]\n"
		"  %s probe-vco [--system DIR] [--user DIR] [--frames N] [--pitch V]\n"
		"\n"
		"Runs Rack's engine headlessly and emits machine-readable JSON.\n",
		argv0, argv0);
}

bool parseInt(const char* s, int* out) {
	char* end = NULL;
	long v = std::strtol(s, &end, 10);
	if (end == s || *end != '\0')
		return false;
	*out = (int) v;
	return true;
}

bool parseFloat(const char* s, float* out) {
	char* end = NULL;
	float v = std::strtof(s, &end);
	if (end == s || *end != '\0' || !std::isfinite(v))
		return false;
	*out = v;
	return true;
}

bool parseOptions(int argc, char** argv, Options* opts) {
	for (int i = 2; i < argc; i++) {
		std::string arg = argv[i];
		if (arg == "--system" && i + 1 < argc) {
			opts->systemDir = argv[++i];
		}
		else if (arg == "--user" && i + 1 < argc) {
			opts->userDir = argv[++i];
		}
		else if (arg == "--plugin" && i + 1 < argc) {
			opts->pluginSlug = argv[++i];
		}
		else if (arg == "--frames" && i + 1 < argc) {
			if (!parseInt(argv[++i], &opts->frames))
				return false;
		}
		else if (arg == "--pitch" && i + 1 < argc) {
			if (!parseFloat(argv[++i], &opts->pitch))
				return false;
		}
		else {
			return false;
		}
	}
	opts->frames = std::max(1, opts->frames);
	return true;
}

void initEnvironment(const Options& opts) {
	settings::headless = true;
	settings::devMode = false;
	settings::safeMode = false;
	settings::sampleRate = 48000.f;
	settings::threadCount = 1;
	asset::systemDir = opts.systemDir;
	asset::userDir = opts.userDir;

	system::init();
	system::resetFpuFlags();
	asset::init();
	logger::logPath = asset::user("rackctl-proto.log");
	logger::init();
	random::init();
	string::init();
	settings::init();
	settings::sampleRate = 48000.f;
	settings::threadCount = 1;

	contextSet(new Context);
	APP->engine = new engine::Engine;
	APP->engine->setSuggestedSampleRate(settings::sampleRate);
	plugin::init();
}

void destroyEnvironment() {
	plugin::destroy();
	delete APP;
	contextSet(NULL);
	settings::destroy();
	logger::destroy();
}

void printJson(json_t* rootJ) {
	json_dumpf(rootJ, stdout, JSON_COMPACT);
	std::printf("\n");
}

int listModels(const Options& opts) {
	initEnvironment(opts);

	json_t* rootJ = json_object();
	json_object_set_new(rootJ, "plugins", json_array());
	json_t* pluginsJ = json_object_get(rootJ, "plugins");

	for (plugin::Plugin* p : plugin::plugins) {
		if (!opts.pluginSlug.empty() && p->slug != opts.pluginSlug)
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

	json_object_set_new(rootJ, "pluginCount", json_integer((json_int_t) json_array_size(pluginsJ)));
	printJson(rootJ);
	json_decref(rootJ);

	destroyEnvironment();
	return 0;
}

int probeVco(const Options& opts) {
	initEnvironment(opts);

	plugin::Model* model = plugin::getModel("Fundamental", "VCO");
	if (!model) {
		std::fprintf(stderr, "ERROR: Fundamental/VCO is not loaded.\n");
		destroyEnvironment();
		return 2;
	}

	engine::Module* module = model->createModule();
	if (!module) {
		std::fprintf(stderr, "ERROR: Fundamental/VCO did not create a module.\n");
		destroyEnvironment();
		return 2;
	}

	module->id = 1;
	APP->engine->addModule(module);

	// Fundamental VCO enum order: MODE, SYNC, FREQ, FINE, FM, PW, PW_CV, LINEAR.
	APP->engine->setParamValue(module, 2, opts.pitch);
	// Rack modules commonly skip output generation for disconnected ports.
	module->outputs[0].channels = 1;

	float minV = INFINITY;
	float maxV = -INFINITY;
	double sumSquares = 0.0;
	int samples = 0;
	int positiveZeroCrossings = 0;
	float previousV = 0.f;

	for (int i = 0; i < opts.frames; i++) {
		APP->engine->stepBlock(1);
		// Fundamental VCO output order: SIN, TRI, SAW, SQR.
		float v = module->outputs[0].getVoltage();
		if (samples > 0 && previousV <= 0.f && v > 0.f)
			positiveZeroCrossings++;
		minV = std::min(minV, v);
		maxV = std::max(maxV, v);
		sumSquares += (double) v * (double) v;
		previousV = v;
		samples++;
	}

	double rms = std::sqrt(sumSquares / std::max(1, samples));
	json_t* rootJ = json_object();
	json_object_set_new(rootJ, "plugin", json_string("Fundamental"));
	json_object_set_new(rootJ, "module", json_string("VCO"));
	json_object_set_new(rootJ, "frames", json_integer(opts.frames));
	json_object_set_new(rootJ, "sampleRate", json_integer(48000));
	json_object_set_new(rootJ, "output", json_string("SIN"));
	json_object_set_new(rootJ, "min", json_real(minV));
	json_object_set_new(rootJ, "max", json_real(maxV));
	json_object_set_new(rootJ, "rms", json_real(rms));
	json_object_set_new(rootJ, "positiveZeroCrossings", json_integer(positiveZeroCrossings));
	printJson(rootJ);
	json_decref(rootJ);

	APP->engine->removeModule(module);
	delete module;
	destroyEnvironment();
	return 0;
}

} // namespace

int main(int argc, char** argv) {
	if (argc < 2) {
		usage(argv[0]);
		return 1;
	}

	std::string command = argv[1];
	Options opts;
	if (!parseOptions(argc, argv, &opts)) {
		usage(argv[0]);
		return 1;
	}

	if (command == "probe-vco") {
		return probeVco(opts);
	}
	if (command == "list-models") {
		return listModels(opts);
	}

	usage(argv[0]);
	return 1;
}
