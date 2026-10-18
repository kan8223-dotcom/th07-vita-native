# Shaders

The VPK carries the compiled programs (`.gxp`); their Cg sources are next to them.
They were compiled with [open-shacccg](https://github.com/frangarcj/open-shacccg)
(MIT; based on commit `26da8eb` with local changes for these programs), not with
Sony's shader compiler. The build only packages the `.gxp` files.

| Program | Source | Source SHA-256 | GXP SHA-256 | Use |
|---|---|---|---|---|
| draw_v.gxp | draw_v.cg | e23241c3…aaade | a866427a…918f5 | vertex program for every draw |
| draw_f.gxp | draw_f.cg | 5533d678…e7583 | 094cf5da…54363 | loaded at start-up (parameter lookup); not used for drawing |
| proven_uniform_f.gxp | proven_uniform_f.cg | c012b7e3…1035b | 4be80ff8…9fa77 | solid colour (uniform `uClearColor`) |
| proven_diffuse_f.gxp | proven_diffuse_f.cg | 539a71bb…9f404 | 4c486928…c586e | interpolated vertex colour (TEXCOORD1) |
| proven_tint_f.gxp | proven_tint_f.cg | b97240c8…26856 | 85fb0396…2c38a | texel × uniform `tint` |

Full hashes: `sha256sum third_party/touhou-modern-vita/shaders/*`.
