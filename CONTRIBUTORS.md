# Contributors

## PromethenDev: project lead

- Started and directs the project.
- Designs the features and the launcher, decides what ships.
- Tests every build on their own PCs and laptop and plays it.

## Claude Code (Anthropic): AI coding assistant

This project was made with [Claude Code](https://claude.com/claude-code). Claude Code wrote most of this project's code
and docs, working under the project lead's direction, and directed the other AI assistants below:

- **Recompilation setup and runtime fixes** for WWE 2K14 on the ReXGlue SDK.
- **Native renderer**: drawing the game from its Direct3D calls with Vulkan, picture sizes, frame-rate modes, and the
  memory-budget fix for integrated graphics.
- **Launcher**: game-file checks, settings, saves, music, the Native / Xbox renderer choice.
- **Tools and docs**: build, packaging and test scripts, README.

## Other AI assistants

Parts of the code, investigations and reviews were done by AI coding assistants run through opencode (OpenAI GPT and
DeepSeek models), with their work reviewed and merged by Claude Code.

## New renderer (v1.7.0)

- **iMrBurn**: the WWE '13 Recomp v1.6 native renderer (Vulkan, Direct3D 12, Direct3D 11), released jointly with
  PromethenDev under GPL-3.0, which WWE 2K14 uses from v1.7.0. It builds on the WWE '12 PC project, re:Blue and plume
  (zolaware), and nefariousjosiah's WWE SmackDown vs. Raw ports.
- **Kaiko (KaikoClanworth1)**: plume's Direct3D 11 backend and shader conversion from the WWE SmackDown vs. Raw 2011 PC
  port, used with permission.

## ReXGlue and Xenia

The [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) (via the
[rexglue-sdk-yukes](https://github.com/HollywoodAkeem/rexglue-sdk-yukes) fork) is the static recompiler and runtime this
project is built on. Its runtime is derived from [Xenia](https://github.com/xenia-project/xenia). See those projects
for their contributors and licenses.
