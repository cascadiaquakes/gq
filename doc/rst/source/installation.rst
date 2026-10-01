Installation
============

Dependencies
------------

To build GQ, install:

* `BLEND 2.0 or newer <https://github.com/cascadiaquakes/blend>`_
* `GMT 6.5 or newer <https://github.com/GenericMappingTools/gmt>`_
* `GDAL development package <https://gdal.org/>`_ (normally installed with
  GMT)
* `NetCDF-C 4.1.3 or newer <https://docs.unidata.ucar.edu/netcdf-c/current/>`_,
  built with netCDF-4/HDF5 support
* CMake 3.15 or newer
* A C compiler

GQ also links directly to the netCDF-C library and uses its netCDF-4/HDF5 API.

To build the documentation, install Python 3, Sphinx, and
``sphinx_rtd_theme``. LaTeX and ``latexmk`` are only needed if you want to
build the PDF. Some data preparation scripts in the examples additionally
require the ``curl`` or GDAL command-line utilities. The Python
requirements are listed in
``doc/rst/requirements.txt`` and can be installed with:

.. code-block:: sh

   python -m pip install -r doc/rst/requirements.txt

If the documentation tools are installed in a conda or virtual environment,
activate that environment before configuring GQ. Alternatively, you canset
``Python3_EXECUTABLE`` and ``SPHINX_BUILD_EXECUTABLE`` in the user
configuration file ``cmake/ConfigUser.cmake``.

Obtain the source
-----------------

Clone the GQ source repository and move into the source directory:

.. code-block:: sh

   git clone https://github.com/cascadiaquakes/gq.git
   cd gq

Configuration
-------------

GQ follows GMT's configuration style. Default settings are in
``cmake/ConfigDefault.cmake``. Local modifications are to be made in
``cmake/ConfigUser.cmake``, copied from the default template:

.. code-block:: sh

   cp cmake/ConfigUserTemplate.cmake cmake/ConfigUser.cmake

Edit ``cmake/ConfigUser.cmake`` before configuring. For example, you can set
the GQ installation directory by editing ``CMAKE_INSTALL_PREFIX``:

.. code-block:: cmake

   set (CMAKE_INSTALL_PREFIX "/path/to/install")

The default values for the tests, examples, and documentation build options 
are ``OFF``. 

Build and install
-----------------

Configure and compile from a separate build directory:

.. code-block:: sh

   mkdir build
   cd build
   cmake ..
   cmake --build .
   cmake --build . --target install

CMake queries the currently installed and active GMT program in the session
for its plugin directory and installs ``gq.so`` there by default, matching 
the output from ``gmt --show-plugindir``. Set ``GQ_INSTALL_PLUGINDIR`` only 
if you wish to override that location, for example when staging a package.
``CMAKE_INSTALL_PREFIX`` separately controls GQ-owned files:
the uninstall helper is placed in ``share/tools``, while the license, notice,
examples, and any built documentation are placed in ``doc``. Finally, note 
that the plugin and installation directories may require write permission.

Local build
-----------

The plugin can be used before installation from the build directory by setting 
the ``GMT_CUSTOM_LIBS`` variable to the absolute path of the compiled plugin 
library:

.. code-block:: sh

   export GMT_CUSTOM_LIBS="/absolute/path/to/build/src/plugins/gq.so"
   gmt merge1d

Tests
-----

If ``GQ_BUILD_TESTS`` is ``ON``, compile and run the integration suite from
the build directory:

.. code-block:: sh

   cmake --build .
   ctest --output-on-failure

Examples
--------

GQ examples are maintained as shell scripts in ``doc/examples``. If
``GQ_BUILD_EXAMPLES`` is ``ON``, a normal build compiles the C helper programs
used by those scripts. The option does not run the examples or download their model
datasets.

Documentation
-------------

If ``GQ_BUILD_DOCS`` is ``ON``, you can build the documentation from the build 
directory:

.. code-block:: sh

   cmake --build . --target docs
   cmake --build . --target docs_html
   cmake --build . --target docs_man
   cmake --build . --target docs_pdf

``docs`` builds HTML and man pages. The PDF, which is large, is built only when
``docs_pdf`` is used and ``latexmk`` is available. Outputs are written
under ``doc`` in the build directory. When documentation support is enabled,
the install step also copies any built HTML manual, man pages, and PDF under
the configured installation prefix.

Uninstall
---------

The install step also installs an uninstall helper. Run it from the
installation prefix:

.. code-block:: sh

   ./share/tools/gq_uninstall.sh

To preview the files that would be removed without actually deleting/uninstalling
them:

.. code-block:: sh

   ./share/tools/gq_uninstall.sh --dry-run

The helper records the configured GMT plugin directory, so it remains usable
after the source and build directories are deleted. Use ``--plugindir PATH``
to supply a different path. Removing files from a system prefix
may require permissions.

The helper removes the GQ plugin, installed metadata, examples, documentation,
man pages, and itself. It does not remove GMT, BLEND, netCDF, or other
dependency files. Use ``--prefix PATH`` if the installed GQ files were moved
to a different location.
