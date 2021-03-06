Here we segment each FOD lobe to identify the number and orientation of fixels in each voxel. The output also contains the apparent fibre density (AFD) value per fixel estimated as the FOD lobe integral (see `here <http://www.sciencedirect.com/science/article/pii/S1053811912011615>`_ for details on FOD segmentation). Note that in the following steps we will use a more generic shortened acronym - Fibre Density (FD) instead of AFD, since the following steps can also apply for other measures of fibre density (see the note below). The terminology is also consistent with our `recent work <https://www.ncbi.nlm.nih.gov/pubmed/27639350>`_::

    foreach * : fod2fixel IN/fod_in_template_space.mif -mask ../template/voxel_mask.mif IN/fixel_in_template_space -afd fd.mif

