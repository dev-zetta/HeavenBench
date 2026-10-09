Cloud texture assets in this directory are adapted from EnvironmentalVisualEnhancements
and its BoulderCo sample configuration:

https://github.com/LGhassen/EnvironmentalVisualEnhancements

The source project and BoulderCo configuration are distributed under the MIT
License. These files are retained as source references from the earlier
texture-projected cloud experiment. The current volumetric cloud density is a
continuous procedural weather field and does not sample them: projecting
`eve1.dds` across the cloud slab produced stretched bands, while implicit mip
selection on the detail maps made the silhouettes blurry.

Included source assets:

- `eve1.dds`
- `detaileve1.dds`
- `uvnoise1.dds`
