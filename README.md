# fuse in-memory, readonly filesystem with file mapping

Accepts two parameters:
* Layout as filename to json, key being the relative path inside the mount (with prefixed `/`), value is the file to load from:
```JSON
{
  "/data/meshes/architecture/riften/docks/rtdockpier02.nif": "/mnt/hdd/christian/skyrim_se/smim_lc/data/meshes/architecture/riften/docks/rtdockpier02.nif",
  "/data/textures/landscape/trees/vurt_barkpine1_n.dds": "/mnt/hdd/christian/skyrim_se/flora_lc/data/textures/landscape/trees/vurt_barkpine1_n.dds"
}
```
* mount point
