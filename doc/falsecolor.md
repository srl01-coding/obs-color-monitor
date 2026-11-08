# False Color

## Introduction

False Color shows intensity in different colors to help adjust exposure.

The false color is hardcoded in an effect file `data/falsecolor.effect`.

RGB color `#000000` corresponds to 0&nbsp;IRE `#FFFFFF` corresponds to 100&nbsp;IRE.

<img src="falsecolor.svg" alt="color table"/>

## HLG canvas (Rec.2100 HLG)

When the OBS canvas is Rec.2100 HLG, the False Color source switches automatically to
bands on the HLG signal level (BT.2020 luma of the 10-bit HLG signal OBS would output;
narrow or full range from the OBS settings). Levels follow Report ITU-R BT.2408
(Tables 1 and 2); nits are for a 1000&nbsp;cd/m&sup2; display.

| HLG signal | Colour | Meaning |
| ---------- | ------ | ------- |
| below 2% | purple | crushed black |
| 2-10% | blue | |
| 10-20% | light blue | |
| 20-36% | dark grey | |
| 36-40% | green | 18% grey card (38%, 26 nits) |
| 40-55% | medium grey | |
| 55-65% | pink | light skin tones (BT.2408 Type 1-2; medium skin is 45-60%, dark 25-45%) |
| 65-73% | light grey | |
| 73-77% | cyan | HDR reference white / graphics white (75%, 203 nits) |
| 77-87% | dark yellow | highlights, about 230-430 nits |
| 87-95% | yellow | about 430-720 nits |
| 95-100% | orange | about 720-1000 nits |
| above 100% | red | super-white: narrow-range codes above 940, beyond the nominal peak |

The key shows 0-100% signal; the red band is beyond the key. A LUT, if enabled, is indexed by
the HLG signal level 0-100%. The False Color filter (as opposed to the source) is not
HLG-aware.

## Properties

### Source
(Not available for False Color Filter.)

Selects one of Program, Main view, Preview, Scene, or Source.
The default is Program.

### Use LUT

Enable if you want to customize the color map.

### LUT file name

If 'Use LUT' is enabled, choose an image file for the color map.
The image file should be 1-pixel height and can have any width.
The left most pixel represents the black and the right most pixel represents the white.

An example is [available here](falsecolor-lut-default.png).

### Show key

A key will be shown.
Available options are as below.

| Option | Description |
| ------ | ----------- |
| None | The key won't be displayed. |
| Left | A vertical key will be displayed on the left side on the source. |
| Right | A vertical key will be displayed on the right side on the source. |
| Outside (Right) | A vertical key will be displayed right outside of the source. |
| Top | A horizontal key will be displayed on the top on the source. |
| Bottom | A horizontal key will be displayed on the bottom on the source. |
| Outside (Bottom) | A horizontal key will be displayed below the source. |

### Color space

Choice of color space; Auto, BT.601, or BT.709.
If Auto, the color space is retrieved from the settings of OBS Studio.
Coefficients to convert from RGB to Luminance will be changed.
Default is Auto.
