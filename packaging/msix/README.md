# The Microsoft Store package

`make-msix.sh` builds `builddir/math42-<version>-x64.msix`: math42, the
GTK 4 runtime it needs, and the tiles the Store shows, in one file.

```sh
packaging/msix/make-msix.sh
```

It wants MSYS2 MinGW64 (`C:\msys64\mingw64\bin` on PATH, for the DLLs
and for `rsvg-convert` and `magick`) and the Windows 10 SDK, which is
where `makeappx`, `makepri` and `signtool` come from.

## What goes in it

The package is laid out like a MinGW prefix, because that is how GLib
and GTK find their own data on Windows -- from where the DLL sits:

    bin/math42.exe and the 60 DLLs it needs, directly or through
        another, as far as they live in the prefix
    share/glib-2.0/schemas/gschemas.compiled
        GSettings ends the process when a schema it is asked for is
        missing, so GTK's own schemas have to travel with it
    share/icons/hicolor/<n>x<n>/apps/net.office42.math42.png
        the window and the about dialog ask for the icon by name.  PNG,
        rendered at build time: GTK 4 reads PNG itself, and rendering
        the SVG at runtime would mean carrying librsvg
    Assets/
        six tiles at five scales, and the small icon at the five sizes
        the taskbar and the file explorer reach for, plated and not
    resources.pri
        the index that ties a tile to the scale it is for

The gdk-pixbuf loaders are deliberately absent: GTK 4 decodes PNG,
JPEG and TIFF by itself, and the notebook draws everything else with
Cairo, so nothing asks for them.

## The identity

`AppxManifest.xml.in` carries the identity the Store assigned; it is
not ours to choose. The version comes from `meson.build`, with a
fourth number of zero, which is what the Store requires.

    Name                    29567TheFreecivProject.Math42
    Publisher               CN=631F98F7-2280-49EE-8EF8-534CC36D09CF
    PublisherDisplayName    Nordstjernen
    Package family          29567TheFreecivProject.Math42_ga6t65cntcpba
    Store listing           https://apps.microsoft.com/detail/9PDC4D7RGLFX

## Looking at it before sending it

The package that goes to the Store is unsigned: Partner Center signs it
with the publisher's certificate on the way in. To install it here it
has to be signed by something this machine trusts, so

```sh
packaging/msix/make-msix.sh --test-sign
```

also writes `math42-<version>-x64-test-signed.msix` and the throwaway
certificate it used. Installing that needs sideloading turned on
(Settings -> System -> For developers -> Developer Mode) and the
certificate trusted, from an administrator PowerShell:

```powershell
Import-Certificate -FilePath builddir\msix\math42-test.cer `
    -CertStoreLocation Cert:\LocalMachine\TrustedPeople
Add-AppxPackage builddir\math42-1.0.0-x64-test-signed.msix
```

and `Remove-AppxPackage 29567TheFreecivProject.Math42_...` to undo it.

Short of installing it, the staged directory is the whole program and
can be run where it stands, which is the thing worth checking:

```sh
builddir/msix/math42/bin/math42.exe --size 900x700 \
    --screenshot out.png examples/tour.m42
```

## Sending it

Upload the unsigned `math42-<version>-x64.msix` to Partner Center under
the Packages step of a submission. Each submission needs a version
higher than the last, so raise the version in `meson.build` first.
