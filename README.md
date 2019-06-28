# data-viewer

## Prerequisites

* *gtk+-3.0*
* *gdk-pixbuf-2.0*
* *gtksourceview-4*

## Building

```
$ git clone --recursive https://github.com/rajamukherji/data-viewer
$ cd data-viewer
$ git clone --recursive https://github.com/wrapl/rabs
$ make -C rabs
$ ./rabs/rabs -c -s -p4
```

## Usage

```
./bin/data-viewer <csv_file> [ -p <image_prefix> ]
```

The first row of *csv_file* must contain a header (i.e. field / column names). 
The first column of *csv_file* must contain paths to images (either absolute, or relative to the working directory).
The remaining columns will be detected as numeric or categorical data.
The first two columns will be used initially as the x-y coordinates, but can be changed afterwards.

| Image | Feature 1 | Feature 2 | Feature 3 |
|---|---|---|---|
| images/image1.png | 1.0 | 0.5 | cat |
| images/image2.png | 0.3 | -0.2 | dog |

**Note:** Currently the data type detection will fail with a column containing categorical data if the first row of that column contains a valid number.    