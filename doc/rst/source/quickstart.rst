Quickstart
==========

Build and install GQ
--------------------

Clone the GQ source repository and move into the source directory:

.. code-block:: bash

   git clone https://github.com/cascadiaquakes/gq.git
   cd gq

Create a local configuration file by copying
``cmake/ConfigUserTemplate.cmake`` to ``cmake/ConfigUser.cmake``:

.. code-block:: bash

   cp cmake/ConfigUserTemplate.cmake cmake/ConfigUser.cmake

Edit ``cmake/ConfigUser.cmake`` before configuring to set the GQ installation
prefix, dependency locations, the GMT plugin directory, or optional tests,
example helpers, and documentation. Then build and install GQ from a separate
build directory:

.. code-block:: bash

   mkdir build
   cd build
   cmake ..
   cmake --build .
   cmake --build . --target install

Verify the installation
-----------------------

After installation, the GQ modules should be available to GMT and appear at the
end of the module list:

.. code-block:: bash

   gmt --show-modules

Optionally, you can run the GMT help command to see the GQ modules listed with 
their descriptions:

.. code-block:: bash

   gmt --help

Display the complete usage for any module by typing the module name following the 
GMT command. For example, to see the usage for the ``merge2d`` module, type:

.. code-block:: bash

   gmt merge2d

If you want to work with an uninstalled build, simply point GMT to the compiled plugin
library by setting the ``GMT_CUSTOM_LIBS`` variable:

.. code-block:: bash

   export GMT_CUSTOM_LIBS="/absolute/path/to/build/src/plugins/gq.so"
   gmt merge2d 

Examples and tutorials
----------------------

Examples are organized by module under ``doc/examples``. Each example
directory contains a README, a classic-mode GMT shell script, and figures.
They range from simple to complex and highlight the breadth of GQ
functionality. Understanding some examples may require familiarity with GMT,
but more than 80 well-documented examples are available to help new users
become familiar with GQ.

Realistic examples use Cascadia
`CRESCENT CVM models <https://cvm.cascadiaquakes.org/>`_, although GQ can use
other NetCDF models, including models from the
`EarthScope EMC <https://ds.iris.edu/ds/products/emc-earthmodels/>`_. Browse
the complete :doc:`examples/index` for the example descriptions and figures.
