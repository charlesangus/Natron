Instructions for installing Natron from sources on GNU/Linux
============================================================

This file is supposed to guide you step by step to have working (compiling) version of Natron on GNU/Linux. Here's the gist of what you need to know:

* It's recommended to use Docker for the easiest hands-off installation method - see [here](#using-docker) for more details
* If you are on Arch Linux or Manjaro, see [this](#arch-linux) for relevant details
* If you are on Fedora or RHEL, see [here](#fedorarhel-based) for specific instructions
* If you are on Debian or a Debian-based system (such as Ubuntu, Linux Mint, KDE Neon, ElementaryOS etc.) see [here](#debian-based) for details
* If you are willing to try the complete installation process, the instructions are below

0. [Using Docker](#using-docker)
1. [Dependencies](#dependencies)
  - [Installing the full Natron SDK](#installing-the-full-natron-sdk)
    - [Environment to use the Natron SDK](#environment-to-use-the-natron-sdk)
  - [Manually install dependencies](#manually-install-dependencies)
    - [Qt5](#qt-515)
    - [Boost](#boost)
    - [Expat](#expat)
    - [Cairo](#cairo)
    - [Pyside2](#pyside2)
    - [Shiboken2](#shiboken2)
2. [Configuration](#configuration)
    - [OpenFX](#openfx)
    - [OpenColorIO-Configs](#download-opencolorio-configs)
    - [Nodes](#nodes)
3. [Build](#build)
4. [Distribution specific](#distribution-specific)
    - [Arch Linux](#arch-linux)
    - [Debian-based](#debian-based)
    - [Fedora/RHEL-based](#fedorarhel-based)
5. [Generating Python bindings](#generating-python-bindings)

# Using Docker

If you have `docker` installed, the installation procedure is very simple. Simply create a directory called `builds`, and then run the following command:

```bash
docker run -it --rm --mount src="$(pwd)/builds",target=/home/builds_archive,type=bind natrongithub/natron-sdk:latest
```

Docker will automatically do the rest for you, and you should have a complete Natron binary in `./builds` (as a tgz archive).

# Dependencies

The dependencies necessary to build and install Natron can either be built specifically for Natron, using the Natron SDK, or installed using packages from the Linux distribution.

## Installing the full Natron SDK

The Natron SDK is used for building the official Natron binaries. The script that builds the whole SDK and installs it in the default location (`/opt/Natron-sdk`, which must be user-writable) can be exectuted like this:

```
cd tools/jenkins
include/scripts/build-Linux-sdk.sh
```

It puts build logs and the list of files installed by each package in the directory `/opt/Natron-sdk/var/log/Natron-Linux-x86_64-SDK` or `/opt/Natron-sdk/var/log/Natron-Linux-i686-SDK`.

Some packages, especially Qt 4.8.7, have Natron-specific patches. Take a look at the SDK script to see which patches are applied to each packages, and what configuration options are used.

The SDK may be updated by pulling the last modifications to the script and re-executing it.

### Environment to use the Natron SDK

Once the SDK is built, you should set your environment in the shell from which you execute or test Natron, to make sure that the Natron SDK is preferred over any other system library:

```
. path_to_Natron_sources/tools/utils/natron-sdk-setup-linux.sh
```

This must be done in every shell/terminal where you intend to compile and/or run Natron.

## Installing dependencies manually

### Qt 5.15

For Qt5 You'll need to install the qtbase libraries, usually you can get them from your package manager (which depends on your Linux distribution).

Alternatively you can build it from source using the tarballs from [Qt download](https://download.qt.io/archive/qt/5.15/5.15.4/submodules) or the [KDE fork](https://invent.kde.org/qt/qt/qtbase/-/tree/kde/5.15).

### Boost

Natron requires `boost serialization` to compile.
You can download boost with your package manager.
Alternatively you can install boost from [boost download](http://www.boost.org/users/download/)

### Expat

You can download it with your package manager.
The package depends on your distribution.

### Cairo

You can download it with your package manager.

### PySide2

Natron uses pyside2 for Python 3 with Qt5.

### Shiboken2

Natron uses shiboken2 for Python 3 with Qt5, the generator binary (`shiboken2`) and headers are required too.

# Configuration

### OpenFX

Natron uses the OpenFX API, before building you should make sure its submodule is up to date.

For that, go under Natron and type

```
git submodule update -i --recursive
```

### Download OpenColorIO-Configs

In the past, OCIO configs were a submodule, though due to the size of the repository, we have chosen instead
to make a tarball release and let you download it [here](https://github.com/NatronGitHub/OpenColorIO-Configs/archive/Natron-v2.4.tar.gz).
Place it at the root of Natron repository.

***note:*** *If it is named something like: `OpenColorIO-Configs-Natron-v2.4` rename it to `OpenColorIO-Configs`*


```
wget https://github.com/NatronGitHub/OpenColorIO-Configs/archive/Natron-v2.4.tar.gz
tar -xvzf Natron-v2.4.tar.gz
mv OpenColorIO-Configs-Natron-v2.4 OpenColorIO-Configs
```

***note:*** In order to reclaim disk space, you may keep only the following subfolders : blender\*, natron, nuke-default

```
cd OpenColorIO-Configs && rm -v !("blender"|"blender-cycles"|"natron"|"nuke-default") -R
```

### Nodes

Natron's nodes are contained in separate repositories. To use the default nodes, you must also build the following repositories:

- [NatronGitHub/openfx-misc](https://github.com/NatronGitHub/openfx-misc)
- [NatronGitHub/openfx-io](https://github.com/NatronGitHub/openfx-io)


You'll find installation instructions in the README of both these repositories. Both openfx-misc and openfx-io have submodules as well.

Plugins can be installed in /usr/OFX/Plugins on Linux
Or in a directory named "Plugins" located in the parent directory where the binary lies, e.g.:

```
bin/
    Natron
Plugins/
    IO.ofx.bundle
```

# Distribution specific

## Arch Linux

On Arch Linux, there are two tested methods of compiling Natron: using the AUR or via manual compiling.

### If using AUR

Simply run the command below:

```
yay -S natron-compositor
```

### If compiling manually

First, install build dependencies. You can install GCC, Expat and Boost directly from the Arch Linux official repositories, like so:

```
sudo pacman -S expat boost-libs gcc
```

You will also need additional Boost libraries, cairo, and Qt5 (provided by PySide2). They can be installed with the following command:

```
sudo pacman -S boost cairo pyside2 python-pyqt
```

Then, clone Natron's repo:

```
git clone https://github.com/NatronGitHub/Natron && cd Natron
```

Update submodules:

```
git submodule init
git submodule update -i --recursive
```

And make a build folder:

```
mkdir build && cd build
```

You're now all set to compile with CMake. See the Debian-based section for CMake build instructions.


## Debian-based

Installing dependencies using `apt-get` or `apt` should work on
any Debian-based distribution.

For Ubuntu 22.04 using Python 3.10 and Qt 5.15, install the required dependencies:

```
sudo apt install build-essential libboost-serialization-dev libboost-system-dev libexpat1-dev libcairo2-dev qtbase5-dev python3-dev libshiboken2-dev libpyside2-dev python3-pyside2.qtwidgets
```

For Debian 12, install the following packages instead:

```
sudo apt install qtbase5-dev libboost-serialization-dev libboost-system-dev libexpat1-dev libcairo2-dev python3-dev python3-pyside2.qtcore libpyside2-dev libshiboken2-dev
```

For most Debian/Ubuntu-based systems, install the required packages:

```
sudo apt install qt5base-dev libboost-serialization-dev libboost-system-dev libexpat1-dev libcairo2-dev python3-dev python3-pyside2 libpyside2-dev libshiboken2-dev
```

Get Natron:

```
git clone https://github.com/NatronGitHub/Natron && cd Natron
git submodule update -i --recursive
wget https://github.com/NatronGitHub/OpenColorIO-Configs/archive/Natron-v2.5.tar.gz
tar xzf Natron-v2.5.tar.gz
mv OpenColorIO-Configs-Natron-v2.5 OpenColorIO-Configs
```

Build:

```
mkdir ../build-Natron && cd ../build-Natron
cmake ../Natron
make -j8
make test
```

## Fedora/RHEL-based

Instructions for Fedora, Red Hat Enterprise Linux and derivatives. You can use either the dnf or yum package managers

On RHEL and derivative distributions you need the EPEL repository:

``` yum install epel-release ``` or ``` dnf install epel-release ```

Install required packages:

```
yum install fontconfig-devel gcc-c++ expat-devel python-pyside2-devel shiboken2-devel qt5-qtbase-devel boost-devel pixman-devel cairo-devel
```
or
```
dnf install fontconfig-devel gcc-c++ expat-devel python-pyside2-devel shiboken2-devel qt5-qtbase-devel boost-devel pixman-devel cairo-devel
```

