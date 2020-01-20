# Debugging Project Verona

We have built several features into the codebase to aid debugging.
This document explains those features.
If anything is unclear, please PR fixes or raise issues.

We will split the rest of this document based on the various stages of running a Verona program

* Compiler
* Interpreter
* Runtime

## Debugging the compiler

The simplist way to debug the compiler is to get it to dump all the phases of compiling every method:
```
veronac.exe --dump-path dmp region101.verona
```
This dumps all the stages of compiling each method into a sub-directory `dmp`.
For each Class and Method, you should find:
```
  [Class].[Method].ast.txt
  [Class].[Method].resolved-ast.txt
  [Class].[Method].elaborated-ast.txt
  [Class].[Method].wf-types.txt
  [Class].[Method].ir.txt
  [Class].[Method].liveness.txt
  [Class].[Method].constraints.txt
  [Class].[Method].infer.txt
  [Class].[Method].solver.txt
  [Class].[Method].substitution.txt
  [Class].[Method].types.txt
  [Class].[Method].typed-ir.txt
  [Class].[Method].make-region-graph.txt
  [Class].[Method].region-graph.txt
```
The order is the order of the phases in the compiler, and can be used to track where something has gone wrong.

Individual, phases and results for particular methods can be output using other options to the compiler.

### Debugging the parser

The Verona parser currently uses [Pegmatite](https://github.com/CompilerTeaching/Pegmatite), a PEG parser design the teaching and rapid prototyping.
This has two compile options that can aid debugging:

* DEBUG_PARSING  - this outputs tracing during parsing the string
* DEBUG_AST_CONSTRUCTION  - this outputs the construction of the AST nodes


## Debugging the interpreter

As the majority of code running in the interpreter is actually the runtime, the subsequent section could also prove useful.
The interpreter provides a trace of all the instructions that it executes.
Every option that you can pass to the interpreter, can also be passed to the compiler prepended with `run-`, for example:
```
interpreter foo.bc --verbose
```
or 
```
veronac foo.verona --run --run-verbose
```
The `--verbose` option basically provides a trace of the interpreters operations:
for example:
```
veronac.exe --run --run-verbose region101.verona
```
produces
```
[ 27b]: Entering function Main.main, argc=1 retc=1 locals=9
[ 28d]: LOAD_DESCRIPTOR r1, 0x1
[ 293]: COPY r8, r1
[ 296]: CALL 7, 0x1
[ 296]:  Calling function Main.test1, base=r8 argc=1 retc=1 locals=37
[ 12c]:  LOAD_DESCRIPTOR r1, 0x3
...
```

Both the interpreter, and the compiler's run option, can also provide systematic testing for concurrency:
```
  veronac-sys.exe --run testsuite/demo/run-pass/dining_phil.verona --run-seed 100 --run-seed_upper 200
```
runs 100 seeds sequentially, but testing various interleavings of the runtime. The seeds are replayable.

## Debugging the runtime

The runtime provides two key features to aid debugging:

* Systematic testing
* Crash logging (A flight recorder that provides the last few seconds of logging)

Both of these can produce high detail logging of the runtime operations.

It is considerably easier to debug a crash in systematic testing, than in standard concurrent execution.
The majority of tests of the runtime cover a large number of pseudo-random traces, so that the failures can easily be replayed.  The code in

* https://github.com/microsoft/verona/blob/master/src/rt/test/harness.h#L38-L70
* https://github.com/microsoft/verona/blob/master/src/rt/test/harness.h#L73-L114

highlights all of the features for running with the various logging and random seed approaches.

To compile the run-time with systematic testing build with
```C++
USE_SYSTEMATIC_TESTING
```
defined, and then call
```C++
Systematic::enable_logging();
```
to print the logging, and
```C++
Scheduler::get().set_seed(seed);
```
specifies the seed for the pseudo-random interleaving.
The seed to interleaving should be consistent across platforms.

To enable crash logging build with
```C++
USE_FLIGHT_RECORDER
```
defined, and call
```C++
Systematic::enable_crash_logging();
```
to enable its output.

The systematic testing log has the earliest entry first, while the crash logging is in reverse, and contains timestamps.
This is a quirk of how the crash log is reconstructed after the runtime crashes.


Enabling ASAN on Clang builds by passing CMake `-DUSE_ASAN=On` will aid in detecting memory corruption issues.
