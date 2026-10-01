merge2d
=======

``merge2d`` tiles or smoothly merges two-dimensional multiparameterNetCDF grids. 
It supports polygonal supports (i.e., clip files) and direct layer selection from 
three-dimensional NetCDF cubes.

Synopsis
--------

.. code-block:: bash

   gmt merge2d <grids|mergefile> -Goutput.nc [-Rregion -Iincrement] [options]

Behavior
--------

Listing at least two grids directly tiles values in availability order. The
first available value is retained by default. A mergefile enables primary and
secondary pairing and contains records of the form::

   primary [secondary] [polygon] [xwindow/ywindow] [rx1/rx2/ry1/ry2]

Each non-comment record contains up to five whitespace-separated fields:

``primary``
   Required NetCDF source providing the primary field or fields.

``secondary``
   Optional source paired with the primary. Pairing applies only within the
   primary support and does not extend beyond the primary domain. Use ``-`` 
   for an unpaired fallback tile.

``polygon``
   Optional xy polygon defining the primary support. The complete primary-grid
   domain is used when the polygon is omitted.

``xwindow/ywindow``
   BLEND window functions for the x and y dimensions. The default is
   ``cosine/cosine``.

``rx1/rx2/ry1/ry2``
   Dimensionless taper ratios in ``[0, 0.5)``. One value applies everywhere,
   two apply symmetrically to x and y, and four control each side
   independently. ``rx1/rx2`` apply at west/east (low/high x), and
   ``ry1/ry2`` apply at south/north (low/high y). Every ratio defaults to 0.2.

   Each ratio sets the fraction of the corresponding support extent used by
   the transition at one boundary. For example, for a rectangular support
   100 coordinate units wide, ``rx1 = 0.2`` gives a west transition
   approximately 20 units wide. Within each transition, the selected window
   controls how the primary merging-weight factor changes between its
   boundary value and 1. The paired secondary receives the complementary weight.
   The x and y factors are multiplied. Larger ratios give broader transitions
   and a smaller full-primary interior. 0 disables the taper on that side.
   The ``boxcar`` window ignores taper ratios and has unit weight throughout
   the support.

   For a polygon support, BLEND evaluates the x and y tapers along local
   cross-sections of the polygon and adapts the transition width where a
   cross-section is too narrow for the nominal support-wide taper.

Use ``-`` to skip an optional field when supplying a later field. Trailing
optional fields may be omitted. Add a secondary as a later unpaired record
when it should provide fallback values outside a paired primary domain. For
example::

   primary.nc?vp secondary.nc?p support.txt cosine/cosine 0.25/0.25/0.25/0.25
   secondary.nc?p - - - -

``-R`` and ``-I`` set the output geometry. When omitted, ``merge2d`` uses the
union of the input domains and their common increments and registration.
Specify ``-I`` when increments differ and ``-r`` when registrations differ.
Inputs that are not co-registered with the output are resampled using GMT
``-n``.

The optional polygon must be strictly xy-monotone after it is mapped to the
output grid. ``-ME`` replaces a non-monotone polygon with its strict envelope,
while ``-MB`` uses the best piecewise strict envelope. Appending ``+w`` writes
the converted polygon with a ``_monotone`` suffix. ``-A`` combines positive
weights where primary supports that share the same secondary source overlap. 
Non-overlapping supports may use different secondaries with this option. 
See `BLEND <https://github.com/cascadiaquakes/blend>`_ for more  information.

Select NetCDF fields by appending ``?`` and a comma-separated list, such as
``model.nc?vp,vs``. Selected fields must share the same horizontal
coordinates. If ``?`` is omitted, the default grid variable is used. The
``-F`` option names output variables. It does not select input fields. Source
selectors map positionally to ``-F``, so ``model1.nc?vp,vs,den`` and
``model2.nc?p,s,d`` can both map to ``vp,vs,rho`` with ``-Fvp,vs,rho``. A
single selected variable keeps its source name when ``-F`` is omitted. Without
a selector or ``-F``, the output field is named ``z``.

A two-dimensional layer can be read directly from a three-dimensional
variable from a NetCDF cube. Square brackets select a zero-based layer index, 
while parentheses select the layer nearest to a coordinate value without vertical 
interpolation::

   model.nc?vp[3],vs[3]
   model.nc?vp(20),vs(20)

Indices begin at zero. Parentheses select the nearest coordinate without vertical 
interpolation. Give one layer selector for every selected field. Coordinate scaling 
precedes coordinate-value selection, while index selection remains positional. Each 
selected layer is materialized temporarily as a 2-D grid and removed after processing.

Input modifiers follow the field list. ``+x``, ``+y``, and ``+z`` scale source
coordinates. ``+v`` supplies one broadcast field scale or one scale per
selected field. Uppercase counterparts set target-unit metadata. ``+n``
declares an additional missing-value sentinel. Use
``file.nc?<field1,field2,...>+<modifiers>`` for named fields or
``file.nc?+<modifiers>`` for the default field. For example,
``model.nc?vp,vs+x0.001+Xkm+y0.001+Ykm+v0.001,0.001+Vkm/s,km/s`` scales x and
y in m to km and both selected fields in m/s to km/s. Input transforms
occur before gap filling, resampling, and merging. Input coordinate scaling
does not reorder grid data. GMT supplies the retained x and y axes in increasing
order, so input ``+x`` and ``+y`` scales must be positive. Use output ``-Z``
when the completed grid should use a decreasing coordinate convention.

The ``-Z`` option independently transforms the completed output. It scales
coordinates and fields in place and does not reorder coordinates, fields, or
weight. A negative axis scale therefore produces a decreasing output axis. For
example, with ``-Fvp,vs``,
``-Z+x0.001+Xkm+y0.001+Ykm+v0.001,0.001+Vkm/s,km/s`` converts x and y in m
to km and both velocity fields in m/s to km/s.

``-H`` fills strictly internal input holes before resampling and merging.
The available methods are nearest neighbor (``n``), linear Delaunay (``l``),
local weighted average (``a``), spline (``s``), and minimum curvature
(``m``). Linear Delaunay (``l``) is the default. A maximum gap span can limit
filling. Original values and missing regions connected to an input boundary
are preserved. ``-P`` can then fill remaining primary NaNs from non-missing
paired secondary values. Primary NaNs are preserved by default.

Declared NetCDF missing values, IEEE NaNs, selector ``+n`` sentinels, and
``-di`` are set to NaN. ``-W`` adds the fixed variable ``weight`` with
``long_name="merging weight"`` and units ``1``. ``-W+o`` writes only the
coordinates and weight. All selected fields use the same weight. Processing
is row-oriented so large output grids do not require every input field to be
simultaneously held in memory.

Usage
-----

Merging-weight output follows the paired supports in mergefile order. The
first support containing a node supplies its primary weight; outside that
support, later supports, including broader parents, remain visible. A zero
on the selected support boundary is retained. With ``-A``, positive weights
are summed and capped at 1 only where primaries overlap and share the selected
secondary. Elsewhere, the regular primary weight is retained. An unpaired
background has weight 0 in merging mode.

This produces one ordered weight field from the supports in the mergefile,
without plotting overlays. It describes the primary tapers, not final
per-source contribution fractions or field-specific missing-value replacements.
The same selection applies to ``-W`` and ``-W+o``.

.. gq-usage:: merge2d

Examples
--------

See :doc:`../examples/merge2d/index`.

See also
--------

:doc:`merge1d`, :doc:`merge3d`, :doc:`../reference/variable-selection`,
:doc:`../reference/scaling-units`
