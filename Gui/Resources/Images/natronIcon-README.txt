# conversion from svg to pdf:
    rsvg-convert -f pdf -o natronIcon.pdf natronIcon.svg

alternatively, the following should work, but prefer the method above:

    inkscape natronIcon.svg --export-pdf=natronIcon.pdf

# icon generation:

density 300 gives a 1708x1708 image, which is large enough for all chosen resolutions.

    convert -background none -density 300 natronIcon.svg -resize 256x256 natronIcon256_linux.png
    optipng -o 7 natronIcon256_linux.png

