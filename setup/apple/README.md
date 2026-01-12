VibeRadiant for Apple OS X
=========================

This directory provides packaging steps for VibeRadiant for OS X. This document describes compiling the application on OSX as well as generating distributable bundles using the framework provided in this directory.

Dependencies & Compilation
--------------------------

Directions for OS X Yosemite 10.10 - your mileage may vary:

- Install [MacPorts](http://macports.org).
- Install [XQuartz](http://xquartz.macosforge.org/)

- Install dependencies with MacPorts:

```
sudo port install dylibbundler pkgconfig gtkglext
```

- Get the VibeRadiant code and compile:

```
git clone https://github.com/themuffinator/VibeRadiant.git
cd VibeRadiant/
make
```

- Run the build:

(from the VibeRadiant/ directory)
```
./install/radiant
```

XQuartz note: on my configuration XQuartz doesn't automatically start for some reason. I have to open another terminal, and run the following command: `/Applications/Utilities/XQuartz.app/Contents/MacOS/X11.bin`, then start radiant. 
    
Building VibeRadiant.app
------------------------

The `Makefile` in the 'setup/apple/' directory will produce a distributable .app bundle for VibeRadiant using `dylibbundler`:

```
make
make image
```

Getting help
------------

IRC: Quakenet #xonotic, or post something on the issue tracker..
