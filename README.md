# GridQuery

[![CI](https://github.com/cascadiaquakes/gq/actions/workflows/ci.yml/badge.svg)](https://github.com/cascadiaquakes/gq/actions/workflows/ci.yml)
[![Documentation](https://github.com/cascadiaquakes/gq/actions/workflows/pages.yml/badge.svg)](https://cascadiaquakes.github.io/gq/)
[![GitHub release](https://img.shields.io/github/v/release/cascadiaquakes/gq)](https://github.com/cascadiaquakes/gq/releases/latest)
[![License](https://img.shields.io/github/license/cascadiaquakes/gq)](https://github.com/cascadiaquakes/gq/blob/main/LICENSE)
[![DOI](https://zenodo.org/badge/DOI/10.5281/zenodo.23091426.svg)](https://doi.org/10.5281/zenodo.23091426)

## What is GQ?

GQ (GridQuery) is a collection of command-line programs for merging multiscale 
and multiresolution data and models, and retrofitting three-dimensional Earth 
models by adding shallow geotechnical layering, modifying their topography and 
bathymetry, and generating or applying statistically defined small-scale 
heterogeneities. Originally developed to support downstream geoscience
research operations within the [CRESCENT CVM Team](https://cascadiaquakes.org/cvm/),
GQ's modules apply to projects across the broader STEM community. GQ leverages
the computational geometry functionality of [BLEND](https://github.com/cascadiaquakes/blend) and 
the mapping machinery of [GMT](https://github.com/GenericMappingTools/gmt) to 
perform its processing tasks. The driving goal of GQ is to provide a flexible and 
accessible platform to trivialize model and data processing tasks usually performed
before their application (e.g., physics-based simulations).

Full documentation: [https://cascadiaquakes.github.io/gq/](https://cascadiaquakes.github.io/gq/)

GQ currently provides eight modules:

| Module | Purpose |
| --- | --- |
| `elygtl` | Apply Ely geotechnical layering to three-dimensional multiparameter NetCDF cubes |
| `merge1d` | Tile or smoothly merge one-dimensional tables and multiparameter NetCDF series |
| `merge2d` | Tile or smoothly merge two-dimensional multiparameter NetCDF grids |
| `merge3d` | Tile or smoothly merge three-dimensional multiparameter NetCDF cubes |
| `ssh1d` | Generate or apply one-dimensional small-scale heterogeneities in tables and multiparameter NetCDF series |
| `ssh2d` | Generate or apply two-dimensional small-scale heterogeneities in multiparameter NetCDF grids |
| `ssh3d` | Generate or apply three-dimensional small-scale heterogeneities in multiparameter NetCDF cubes |
| `topobath` | Add, remove, or replace topography and/or bathymetry in three-dimensional multiparameter NetCDF cubes |

## Dependencies

Building GQ requires:

* [BLEND 2.0 or newer](https://github.com/cascadiaquakes/blend)
* [GMT 6.5 or newer](https://github.com/GenericMappingTools/gmt)
* [GDAL development package](https://gdal.org/) (normally installed with GMT)
* [netCDF-C 4.1.3 or newer](https://docs.unidata.ucar.edu/netcdf-c/current/),
  built with netCDF-4/HDF5 support
* CMake 3.15 or newer
* A C compiler

Building the documentation requires Python 3, Sphinx, and
`sphinx_rtd_theme`. A LaTeX installation with `latexmk` is required to build
the PDF manual. Some example data-preparation scripts additionally require
the `curl` or GDAL command-line utilities.

## Building and installation

Clone the GQ source repository and move into the source directory:

```sh
git clone https://github.com/cascadiaquakes/gq.git
cd gq
```

Create a local configuration file by copying `cmake/ConfigUserTemplate.cmake`
to `cmake/ConfigUser.cmake`. Edit `cmake/ConfigUser.cmake` before configuring 
to set the GQ installation prefix, dependency locations, the GMT plugin directory, 
or optional tests, example helpers, and documentation.

```sh
cp cmake/ConfigUserTemplate.cmake cmake/ConfigUser.cmake
```

Then build and install from a separate build directory:

```sh
mkdir build
cd build
cmake ..
cmake --build .
cmake --build . --target install
```


## Examples/Tutorials

Examples are organized by module under `doc/examples`. Each example directory
contains a README, a classic-mode GMT shell script, and figures. They range 
from simple to complex, highlighting the breadth of GQ functionality. 
Understanding the examples may sometimes require some familiarity with GMT.
However, the over 80 well-documented examples will help new users become familiar
with GQ. Realistic highlight Cascadia [CRESCENT CVM models](https://cvm.cascadiaquakes.org/). 
However, any NetCDF model can be utilized, e.g., models from the 
[EarthScope EMC](https://ds.iris.edu/ds/products/emc-earthmodels/).

## Uninstalling

The installation step also installs an uninstall script. Run it from the
installation top directory:

```sh
./share/tools/gq_uninstall.sh
```

To preview the files to be removed without uninstalling:

```sh
./share/tools/gq_uninstall.sh --dry-run
```

## License

GQ is licensed under the GNU Lesser General Public License, version 3 or any
later version. See `LICENSE` and `NOTICE` for details.

## Citation

If you use GQ in your work, please cite the following references:

1. Ajala, R., & Persaud, P. (2021).
   [Effect of merging multiscale models on seismic wavefield predictions near
   the southern San Andreas fault](https://scholar.google.com/scholar?q=Effect+of+merging+multiscale+models+on+seismic+wavefield+predictions+near+the+southern+San+Andreas+fault).
   `Journal of Geophysical Research: Solid Earth`, 126, 1-23.

2. Ajala, R. (2021).
   [Modified UCVM software with blending functionality](https://doi.org/10.5281/zenodo.4533337).
   `Zenodo`.

3. Ajala, R., & Persaud, P. (2022).
   [Ground-motion evaluation of hybrid seismic velocity models](https://scholar.google.com/scholar?q=Ground-motion+evaluation+of+hybrid+seismic+velocity+models).
   `The Seismic Record`, 2, 186-196.

4. Ajala, R., Persaud, P., & Juarez, A. (2022).
   [Earth model space exploration in Southern California: Influence of 
   topography, geotechnical layer, and attenuation on wavefield accuracy](https://doi.org/10.3389/feart.2022.964806).
   `Frontiers in Earth Science`, 1-19.

5. Ajala, R., Kolawole, F., Share, P. E., Sahakian, V., Delph, J. R.,
   Hooft, E., & He, B. (2025).
   [Toward an accessible framework for synthesizing solid earth models across
   multiple scales](https://www.researchgate.net/publication/392232576_Toward_an_Accessible_Framework_for_Synthesizing_Solid_Earth_Models_Across_Multiple_Scales).
   In `Seismological Society of America Annual Meeting`, Baltimore, Maryland,
   USA, vol. 96, p. 1364.

## Acknowledgment
The development of GQ was funded by [CRESCENT](https://cascadiaquakes.org) and [NSF](https://www.nsf.gov).
