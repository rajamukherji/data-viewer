# data-viewer

## Prerequisites

* *gtk+-3.0*
* *gdk-pixbuf-2.0*

## Building

```
git clone --recursive https://github.com/wrapl/rabs
make -C rabs
./rabs/rabs -c -s -p4
```

## Usage

```
./bin/viewer <csv_file>
```

The first row of *csv_file* must contain a header (i.e. field / column names). 
The first column of *csv_file* must contain paths to images (either absolute, or relative to the working directory).
The remaining columns will be detected as numeric or categorical data.

**Note:** Currently the data type detection will fail with a column containing categorical data if the first row of that column contains a valid number.    