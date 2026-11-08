# Zebra

## Introduction

Zebra indicates bright areas to help adjust exposure.
This plugin provides lower threshold and upper threshold.

## Properties

### Source
(Not available for Zebra Filter.)

Selects one of Program, Main view, Preview, Scene, or Source.
The default is Program.

### Threshold (lower, higher)

Specify the level of the intensity where the zebra should be displayed.
The default is `75%` and `100%`.
The values are inclusive; the zebra will be also shown even if the intensity is same as the specified threshold.
RGB color `#000000` corresponds to 0% `#FFFFFF` corresponds to 100%.

On a Rec.2100 HLG canvas the Zebra source compares the thresholds with the HLG signal level
(BT.2020 luma, 0-100%, narrow-range offset removed): 75% is HDR reference white (203 nits),
100% is the nominal peak (code 940). The upper threshold goes to 110% so super-white
(codes above 940) can be striped. The picture under the stripes is the HLG signal shown
directly, which is how HLG looks on an SDR display. The Zebra filter is not HLG-aware.

### Color space

Choice of color space; Auto, BT.601, or BT.709.
If Auto, the color space is retrieved from the settings of OBS Studio.
Coefficients to convert from RGB to Luminance will be changed.
Default is Auto.
