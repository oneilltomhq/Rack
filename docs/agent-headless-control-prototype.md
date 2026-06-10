# Agent Headless Control Prototype

This branch contains a first proof-of-concept for making Rack operable by an
agent through source-level commands instead of GUI automation.

The prototype is intentionally small. It is not a finished `rackctl`, JSON-RPC
server, patch renderer, or public API. It proves the core technical path:

- boot Rack in a headless process or normal UI process
- keep a live Rack process open behind a JSON-RPC stdin/stdout endpoint
- load an installed plugin
- create a module from a `plugin::Model`
- set a parameter with the same engine primitive used below UI control paths
- step the Rack engine manually
- read module output voltages
- emit machine-readable JSON

## Files

- `adapters/rackctl-proto.cpp`
  - CLI prototype.
  - Currently exposes two commands: `list-models` and `probe-vco`.
  - `list-models` loads Rack plugins and reports plugin/model metadata.
  - `probe-vco` loads `Fundamental/VCO`, sets its frequency parameter, runs the
    engine, and reports sine-output statistics.
- `include/agent.hpp`
  - Internal live-control hook.
- `src/agent.cpp`
  - Newline-delimited JSON-RPC 2.0 control loop over stdin/stdout.
  - Queues commands from a reader thread and executes them on Rack's main/UI
    thread through `agent::process()`.
- `adapters/standalone.cpp`
  - Adds `--agent-control`.
  - Starts the live JSON-RPC endpoint in either headless or normal UI mode.
- `src/app/Scene.cpp`
  - Drains pending agent commands from `Scene::step()` while the Rack UI is
    running, so module creation can be visible to a human observer.
- `Makefile`
  - Adds the `rackctl-proto` build target.
- `.gitignore`
  - Ignores the generated `rackctl-proto` binary.

## Build Prerequisites

This was tested on Fedora 43. A fresh Rack checkout needed Rack's normal build
dependencies plus a few packages that were missing from the machine:

```bash
sudo dnf install -y \
  libXrandr-devel libXinerama-devel libXcursor-devel libXi-devel \
  perl-FindBin perl-IPC-Cmd perl-File-Compare perl-File-Copy \
  autoconf automake libtool \
  jack-audio-connection-kit-devel pulseaudio-libs-devel \
  libstdc++-static
```

The Rack dependency bootstrap also expects initialized submodules:

```bash
git submodule update --init --recursive
make -C Rack dep -j2
```

During bootstrap, a few submodule working trees had been left with all tracked
files deleted (`dep/rtmidi`, `dep/rtaudio`, `dep/fuzzysearchdatabase`). They
were restored with targeted `git submodule update --init --checkout --force`
commands before resuming `make dep`.

## Build

From `/home/tom/src/vendor/VCVRack`:

```bash
make -C Rack rackctl-proto -j2
make -C Fundamental RACK_DIR=/home/tom/src/vendor/VCVRack/Rack -j2
```

`rackctl-proto` links against `Rack/libRack.so`. `Fundamental/plugin.so` is
loaded through Rack's normal plugin loader.

## Plugin Staging

Rack looks for user plugins in a platform-specific user directory:

```text
plugins-lin-x64/
```

For the prototype run, the local Fundamental checkout was staged with a symlink:

```bash
rm -rf /tmp/rack-agent-user
mkdir -p /tmp/rack-agent-user/plugins-lin-x64
ln -s /home/tom/src/vendor/VCVRack/Fundamental \
  /tmp/rack-agent-user/plugins-lin-x64/Fundamental
```

## Run

From `/home/tom/src/vendor/VCVRack/Rack`:

### Creative Session Quickstart

Use this when picking up the agent-driven Rack workflow for creative work.

1. Build Rack and Fundamental:

```bash
make -C /home/tom/src/vendor/VCVRack/Rack -j2 Rack rackctl-proto
make -C /home/tom/src/vendor/VCVRack/Fundamental \
  RACK_DIR=/home/tom/src/vendor/VCVRack/Rack -j2
```

2. Stage Fundamental in a disposable user dir:

```bash
rm -rf /tmp/rack-agent-user
mkdir -p /tmp/rack-agent-user/plugins-lin-x64
ln -s /home/tom/src/vendor/VCVRack/Fundamental \
  /tmp/rack-agent-user/plugins-lin-x64/Fundamental
```

3. Launch Rack with the agent endpoint:

```bash
cd /home/tom/src/vendor/VCVRack/Rack
./Rack --agent-control \
  --system /home/tom/src/vendor/VCVRack/Rack \
  --user /tmp/rack-agent-user
```

4. Send newline-delimited JSON-RPC requests to Rack's stdin. This is the
validated starter patch: blank rack, VCO, Audio 2, Scope, VCO sine to audio
left/right, and VCO sine to scope channel 1.

```json
{"jsonrpc":"2.0","id":1,"method":"clear"}
{"jsonrpc":"2.0","id":2,"method":"create_module","params":{"id":"vco1","plugin":"Fundamental","model":"VCO","pos":[0,0]}}
{"jsonrpc":"2.0","id":3,"method":"create_module","params":{"id":"audio1","plugin":"Core","model":"AudioInterface2","pos":[8,0]}}
{"jsonrpc":"2.0","id":4,"method":"create_module","params":{"id":"scope1","plugin":"Fundamental","model":"Scope","pos":[12,0]}}
{"jsonrpc":"2.0","id":5,"method":"configure_audio","params":{"module":"audio1","driver":-1,"sampleRate":48000,"blockSize":256}}
{"jsonrpc":"2.0","id":6,"method":"set_param","params":{"module":"vco1","param":2,"value":0}}
{"jsonrpc":"2.0","id":7,"method":"connect","params":{"outputModule":"vco1","output":0,"inputModule":"audio1","input":0}}
{"jsonrpc":"2.0","id":8,"method":"connect","params":{"outputModule":"vco1","output":0,"inputModule":"audio1","input":1}}
{"jsonrpc":"2.0","id":9,"method":"connect","params":{"outputModule":"vco1","output":0,"inputModule":"scope1","input":0}}
{"jsonrpc":"2.0","id":10,"method":"list_modules"}
```

Known numeric IDs for this starter patch:

- Fundamental `VCO`: frequency param `2`, sine output `0`.
- Core `AudioInterface2`: device-output inputs `0` and `1`.
- Fundamental `Scope`: channel 1 input `0`, channel 2 input `1`.

If the patch appears but there is no sound, select an audio device manually in
the Audio module. `configure_audio` uses Rack's first available driver/default
device when `driver` is `-1`, but local audio setup can still fail or choose an
unwanted backend.

### Live Rack Process

Run Rack with the JSON-RPC control endpoint enabled:

```bash
./Rack \
  --agent-control \
  --system /home/tom/src/vendor/VCVRack/Rack \
  --user /tmp/rack-agent-user
```

This launches the normal Rack UI and also reads newline-delimited JSON-RPC 2.0
requests from stdin. Commands are applied to the live Rack process; a human can
watch module creation and parameter changes in the window.

The same endpoint can run headlessly:

```bash
./Rack --headless --agent-control \
  --system /home/tom/src/vendor/VCVRack/Rack \
  --user /tmp/rack-agent-user
```

Example request stream:

```json
{"jsonrpc":"2.0","id":1,"method":"list_models","params":{"plugin":"Fundamental"}}
{"jsonrpc":"2.0","id":2,"method":"create_module","params":{"id":"vco1","plugin":"Fundamental","model":"VCO","pos":[0,0]}}
{"jsonrpc":"2.0","id":3,"method":"set_param","params":{"module":"vco1","param":2,"value":12}}
{"jsonrpc":"2.0","id":4,"method":"list_modules"}
```

Supported methods in this first live prototype:

- `list_models`
- `clear`
- `create_module`
- `set_param`
- `connect`
- `configure_audio`
- `list_modules`

### Live RPC Reference

All requests are JSON-RPC 2.0 objects, one per line. Responses are also one JSON
object per line.

`list_models`:

```json
{"jsonrpc":"2.0","id":1,"method":"list_models","params":{"plugin":"Fundamental"}}
```

`clear`:

```json
{"jsonrpc":"2.0","id":2,"method":"clear"}
```

`create_module`:

```json
{"jsonrpc":"2.0","id":3,"method":"create_module","params":{"id":"vco1","plugin":"Fundamental","model":"VCO","pos":[0,0]}}
```

The optional `id` is an agent alias used by later commands. `pos` is Rack grid
position, not pixels.

`set_param`:

```json
{"jsonrpc":"2.0","id":4,"method":"set_param","params":{"module":"vco1","param":2,"value":12}}
```

`connect`:

```json
{"jsonrpc":"2.0","id":5,"method":"connect","params":{"outputModule":"vco1","output":0,"inputModule":"audio1","input":0}}
```

`configure_audio`:

```json
{"jsonrpc":"2.0","id":6,"method":"configure_audio","params":{"module":"audio1","driver":-1,"sampleRate":48000,"blockSize":256}}
```

`list_modules`:

```json
{"jsonrpc":"2.0","id":7,"method":"list_modules"}
```

### Creative Workflow Notes

- Start every exploratory patch with `clear` unless you intentionally want to
  build on the current Rack state.
- Create `AudioInterface2` early and wire at least one source to inputs `0` and
  `1` so the patch has an audible output path.
- Add `Scope` early and patch a copy of the signal into input `0`; this gives
  visual feedback even when the audio backend is not open.
- Use stable aliases such as `vco1`, `scope1`, `audio1`, `lfo1`, and `vcf1`.
  Aliases are local to the running Rack process and are cleared by `clear`.
- The current API requires numeric param/input/output IDs. Use source files or
  `configParam` / `configInput` / `configOutput` order to find IDs until
  name-to-index lookup exists.
- Keep Rack open while iterating. The endpoint controls the live process, so
  later commands mutate the visible patch instead of creating a new one.

Example blank audible patch:

```json
{"jsonrpc":"2.0","id":1,"method":"clear"}
{"jsonrpc":"2.0","id":2,"method":"create_module","params":{"id":"vco1","plugin":"Fundamental","model":"VCO","pos":[0,0]}}
{"jsonrpc":"2.0","id":3,"method":"create_module","params":{"id":"audio1","plugin":"Core","model":"AudioInterface2","pos":[8,0]}}
{"jsonrpc":"2.0","id":4,"method":"configure_audio","params":{"module":"audio1","driver":-1,"sampleRate":48000,"blockSize":256}}
{"jsonrpc":"2.0","id":5,"method":"set_param","params":{"module":"vco1","param":2,"value":0}}
{"jsonrpc":"2.0","id":6,"method":"connect","params":{"outputModule":"vco1","output":0,"inputModule":"audio1","input":0}}
{"jsonrpc":"2.0","id":7,"method":"connect","params":{"outputModule":"vco1","output":0,"inputModule":"audio1","input":1}}
```

`configure_audio` asks Rack's Audio module to use the first available audio
driver and its default device when `driver` is `-1`. If Rack cannot open a local
audio device, the visible patch is still created but no sound will be heard
until a device is selected in the Audio module.

### One-Shot Probe CLI

List loaded models:

```bash
./rackctl-proto list-models \
  --system /home/tom/src/vendor/VCVRack/Rack \
  --user /tmp/rack-agent-user \
  --plugin Fundamental
```

Example output, shortened for readability:

```json
{"plugins":[{"slug":"Fundamental","name":"VCV Free","brand":"VCV","version":"2.6.5.rc1","models":[{"slug":"VCO","name":"VCO","hidden":false},{"slug":"VCO2","name":"Wavetable VCO","hidden":false}],"modelCount":39}],"pluginCount":1}
```

The actual command prints every loaded model for the selected plugin. Omitting
`--plugin Fundamental` returns every loaded plugin.

Probe the Fundamental VCO:

```bash
./rackctl-proto probe-vco \
  --system /home/tom/src/vendor/VCVRack/Rack \
  --user /tmp/rack-agent-user \
  --frames 48000 \
  --pitch 0
```

Example output:

```json
{"plugin":"Fundamental","module":"VCO","frames":48000,"sampleRate":48000,"output":"SIN","min":-5.24486113,"max":4.97898388,"rms":3.51999955,"positiveZeroCrossings":261}
```

Changing the frequency parameter changes the measured output. One octave up:

```bash
./rackctl-proto probe-vco \
  --system /home/tom/src/vendor/VCVRack/Rack \
  --user /tmp/rack-agent-user \
  --frames 48000 \
  --pitch 12
```

Example output:

```json
{"plugin":"Fundamental","module":"VCO","frames":48000,"sampleRate":48000,"output":"SIN","min":-5.14843178,"max":4.98984146,"rms":3.52836663,"positiveZeroCrossings":523}
```

The doubled zero-crossing count is the quick sanity check that the parameter
change reached the DSP path.

## Important Implementation Notes

Rack does not appear to ship a built-in JSON-RPC or CLI control API for a
running headless engine. Its existing `--headless` path starts enough of Rack to
run without the UI, but it does not expose a command interface for creating
modules, setting params, wiring cables, or reading outputs.

This branch adds a prototype JSON-RPC control path. It is intentionally
process-local and stdio-based for now. It avoids network binding and is easy for
an agent to drive as a child process.

For normal UI mode, commands are read on a background thread but executed from
`Scene::step()`. This matters because creating visible modules touches the app
widget tree. Directly mutating Rack UI widgets from the reader thread would be
unsafe.

The prototype uses Rack's existing C++ primitives directly:

- `plugin::init()` loads Core and user plugins.
- `plugin::plugins` exposes the loaded plugin list used by `list-models`.
- `plugin::getModel("Fundamental", "VCO")` finds the module model.
- `model->createModule()` instantiates a module.
- `model->createModuleWidget(module)` creates visible modules for UI mode.
- `APP->scene->rack->addModule(moduleWidget)` attaches visible modules to the
  rack widget tree.
- `APP->engine->addModule(module)` attaches it to the engine.
- `APP->engine->setParamValue(module, paramId, value)` sets the parameter.
- `APP->engine->stepBlock(1)` advances DSP.
- `module->outputs[outputId].getVoltage()` reads output voltage.

One Rack-specific detail matters: many modules skip generating an output unless
the output port is connected. In Rack's `Port`, `channels > 0` is the connected
marker. Because this prototype does not yet create a real cable to a sink module,
it marks the VCO sine output as connected with:

```cpp
module->outputs[0].channels = 1;
```

That is prototype-grade. A fuller implementation should create actual cables or
provide a small sink module that owns the output capture path.

The sample rate is forced after `settings::init()` because loading settings can
overwrite earlier defaults:

```cpp
settings::sampleRate = 48000.f;
APP->engine->setSuggestedSampleRate(settings::sampleRate);
```

The prototype writes Rack logs to `<user-dir>/rackctl-proto.log` with the same
`logger::logPath = asset::user(...)` pattern used by the standalone app. This
keeps stdout reserved for machine-readable JSON and stderr reserved for CLI
errors.

## What This Proves

This proves that an agent-friendly Rack interface can be built without GUI
automation. The agent can drive Rack through deterministic commands that map to
engine operations and return structured output.

It also proves the more useful human/agent workflow: Rack can run with its
normal UI while an agent controls the same live process from behind a structured
command channel.

The smallest useful next shape is a `rackctl` command layer with operations like:

- `list-plugins`
- `connect`
- `step`
- `read-output`
- `load-patch`
- `save-patch`
- `render`

A fuller JSON-RPC dispatcher should grow from the current live stdio endpoint:

```json
{"op":"create_module","id":"vco1","plugin":"Fundamental","model":"VCO"}
{"op":"set_param","module":"vco1","param":"FREQ_PARAM","value":12}
{"op":"step","frames":48000}
{"op":"read_output","module":"vco1","output":"SIN_OUTPUT","stats":["rms","zero_crossings"]}
```

## Current Limitations

- The live JSON-RPC endpoint supports `list_models`, `clear`, `create_module`,
  `set_param`, `connect`, `configure_audio`, and `list_modules`.
- The one-shot `rackctl-proto` helper still only supports `list-models` and
  `probe-vco`.
- The one-shot `probe-vco` path still hard-codes Fundamental VCO param/output
  IDs.
- The live endpoint still requires numeric param IDs; it does not yet expose
  name-to-index lookup.
- There is no audio file rendering or live audio capture yet.
- The one-shot output capture currently fakes connection state instead of using
  real cables.
- Rack logs are redirected to `rackctl-proto.log`, but CLI errors still use
  stderr.
- The live endpoint is stdio JSON-RPC only. There is no HTTP server or socket
  transport yet.
- Live UI mutations are queued to the main thread, but this is still a
  prototype and has not been hardened for concurrent human edits.
- This is a source-tree prototype, not a polished downstream package.

## Version Control State

The prototype work is isolated on branch:

```bash
agent-headless-control-prototype
```

At the time this document was written, the meaningful source changes are:

- `.gitignore`
- `Makefile`
- `adapters/standalone.cpp`
- `adapters/rackctl-proto.cpp`
- `include/agent.hpp`
- `src/agent.cpp`
- `src/app/Scene.cpp`
- `docs/agent-headless-control-prototype.md`

The generated `rackctl-proto` binary is ignored and should not be committed.

## Suggested Next Thread

Start by hardening the live JSON-RPC endpoint:

1. Add name-to-index lookup for params, inputs, and outputs.
2. Add an explicit audio-driver/device listing and selection API.
3. Add a capture sink so output reading uses real Rack cable semantics.
4. Decide whether to add a local HTTP or socket transport alongside stdio.
5. Share the dispatcher with one-shot CLI commands instead of keeping
   `rackctl-proto` separate.
