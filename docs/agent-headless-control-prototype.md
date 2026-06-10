# Agent Headless Control Prototype

This branch contains a first proof-of-concept for making Rack operable by an
agent through source-level commands instead of GUI automation.

The prototype is intentionally small. It is not a finished `rackctl`, JSON-RPC
server, patch renderer, or public API. It proves the core technical path:

- boot Rack in a headless process
- load an installed plugin
- create a module from a `plugin::Model`
- set a parameter with the same engine primitive used below UI control paths
- step the Rack engine manually
- read module output voltages
- emit machine-readable JSON

## Files

- `adapters/rackctl-proto.cpp`
  - CLI prototype.
  - Currently exposes one command: `probe-vco`.
  - Loads `Fundamental/VCO`, sets its frequency parameter, runs the engine, and
    reports sine-output statistics.
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

The prototype uses Rack's existing C++ primitives directly:

- `plugin::init()` loads Core and user plugins.
- `plugin::getModel("Fundamental", "VCO")` finds the module model.
- `model->createModule()` instantiates a module.
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

## What This Proves

This proves that an agent-friendly Rack interface can be built without GUI
automation. The agent can drive Rack through deterministic commands that map to
engine operations and return structured output.

The smallest useful next shape is a `rackctl` command layer with operations like:

- `list-plugins`
- `list-models`
- `create-module`
- `set-param`
- `connect`
- `step`
- `read-output`
- `load-patch`
- `save-patch`
- `render`

A JSON-lines command loop or local JSON-RPC server would let an agent keep a Rack
engine process alive and mutate it incrementally:

```json
{"op":"create_module","id":"vco1","plugin":"Fundamental","model":"VCO"}
{"op":"set_param","module":"vco1","param":"FREQ_PARAM","value":12}
{"op":"step","frames":48000}
{"op":"read_output","module":"vco1","output":"SIN_OUTPUT","stats":["rms","zero_crossings"]}
```

## Current Limitations

- Only one hard-coded command exists: `probe-vco`.
- Module IDs, param IDs, and output IDs are hard-coded for Fundamental VCO.
- There is no generic patch graph, cable creation, audio file rendering, or live
  RPC protocol yet.
- Output capture currently fakes connection state instead of using real cables.
- Rack logs still go to stdout/stderr alongside JSON output.
- This is a source-tree prototype, not a polished downstream package.

## Version Control State

The prototype work is isolated on branch:

```bash
agent-headless-control-prototype
```

At the time this document was written, the meaningful source changes are:

- `.gitignore`
- `Makefile`
- `adapters/rackctl-proto.cpp`
- `docs/agent-headless-control-prototype.md`

The generated `rackctl-proto` binary is ignored and should not be committed.

## Suggested Next Thread

Start by turning `probe-vco` into a minimal generic command surface:

1. Add `list-models` to prove plugin/model discovery.
2. Add JSON output without Rack log noise.
3. Add a tiny in-process graph model: create modules, connect outputs to inputs,
   set params, step frames.
4. Add a capture sink so output reading uses real Rack cable semantics.
5. Decide whether the interface should be:
   - one-shot CLI commands
   - a JSON-lines stdin/stdout loop
   - a local HTTP JSON-RPC server
   - all of the above sharing the same command dispatcher

