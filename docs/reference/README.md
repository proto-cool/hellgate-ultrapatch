# Reference

Facts recovered from the binary, the archives and elsewhere. Each file
states where its facts come from.

| File | Contents |
|---|---|
| [exe-facts.md](exe-facts.md) | the Steam executable: build, hashes, sections, symbols |
| [renderer.md](renderer.md) | the D3D9 renderer: technique selection, lighting model, shadows, environment data |
| [stock-shaders.md](stock-shaders.md) | the stock `.fxo` effects: tiers, feature bits, where lighting and fog happen |
| [data-tunables.md](data-tunables.md) | what the cooked data files let you change (lights, particles, environments) |
| [script-actions.md](script-actions.md) | the script action table, with handler addresses |
| [russian-patch-analysis.md](russian-patch-analysis.md) | teardown of the third-party "2026 fix" executable |
| [community-research.md](community-research.md) | community complaints, existing mods and fixes (web research, unverified) |

The session logs behind the 1 FPS findings are in [../logs/](../logs/);
[../codemap/](../codemap/) lists the executable's functions by source file.
