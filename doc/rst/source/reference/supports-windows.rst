Supports, Windows, and Aggregate Merging
========================================

Mergefile records
-----------------

The merge modules read one primary support per mergefile record. Use ``-`` to
use a default option while supplying a later field.

For ``merge1d``:

.. code-block:: text

   primary [secondary] [west/east] [window] [r1/r2]

For ``merge2d``:

.. code-block:: text

   primary [secondary] [polygon] [xwindow/ywindow] [rx1/rx2/ry1/ry2]

For ``merge3d``:

.. code-block:: text

   primary [secondary] [polygon] [zlo/zhi] [xwindow/ywindow/zwindow] \
       [rx1/rx2/ry1/ry2/rz1/rz2]

An omitted interval or polygon uses the full primary domain. Each record is
valid only inside that primary support. A paired secondary supplies the
transition partner, but does not become a tile outside the primary domain 
unless listed in the mergefile.

Windows
-------

The default is a cosine window with a taper ratio of 0.2 at every supported
boundary. A taper ratio is a dimensionless fraction that controls the width
of the transition measured inward from that boundary. The selected window
controls the shape of the transition: the primary merging-weight factor
changes between the window's boundary value and 1, and a paired secondary
receives the complementary weight. Larger ratios produce broader transitions
and leave a smaller full-primary interior. A ratio of 0 disables the taper on
that side. The ``boxcar`` window ignores taper ratios and has unit weight
throughout the support.

Beginning and ending mean low and high coordinate, respectively. Thus, the x
ratios apply at ``west/east``, the y ratios at ``south/north``,`` and the z 
ratios at ``zlo/zhi``. One ratio applies everywhere, dimension-count ratios 
apply symmetrically by dimension, and twice the dimension count sets beginning 
and ending tapers independently. Each ratio must be in ``[0, 0.5)``, i.e., can
approach 0.5 but cannot equal 0.5.

For an interval or rectangular support, a ratio ``r`` uses approximately
``r`` times the support extent for that boundary's transition. For polygon
supports, BLEND evaluates the x and y tapers along local polygon
cross-sections and adapts the transition width where a cross-section is too
narrow for the nominal support-wide taper. In three dimensions, the z taper
uses the complete extruded/extended ``zlo/zhi`` interval. Multidimensional 
taper factors are multiplied to obtain the primary merging weight, i.e., 
outer product of the window taper functions in each dimension.

Polygon supports must be xy-monotone after mapping to the output grid. The
merge and SSH modules provide strict-envelope and best piecewise-envelope
conversion modes. Their write modifier stores the converted polygon beside
the original for inspection. See `BLEND <https://github.com/cascadiaquakes/blend>`_ 
for more  information.

Aggregate merging
-----------------

Without aggregate mode, GQ follows mergefile availability order. Aggregate
mode addresses edge effects where positive primary supports overlap. It
normalizes and combines only the weights of overlapping primaries that share
the same secondary pair.

Primaries with non-overlapping supports do not need a common secondary.
Unpaired records remain ordered fallback tiles and do not enter aggregate
weight normalization. A file that serves as a secondary on one record may
still appear separately as a primary or fallback tile elsewhere.

SSH localization
----------------

SSH supports use the same BLEND window machinery, but multiply the window by
the heterogeneity field rather than blending primary and secondary data. The
stationary field is normalized before tapering and is not normalized again
afterward.
