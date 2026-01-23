# PWA Icon Requirements

For the Progressive Web App to function properly, you need to create two icon files:

## Required Files

1. **icon-192.png** - 192x192 pixels
2. **icon-512.png** - 512x512 pixels

## How to Create Icons

### Option 1: Online Icon Generator
1. Go to https://realfavicongenerator.net/
2. Upload a logo or create a simple icon
3. Generate and download 192x192 and 512x512 PNG files
4. Save them in the `web/` directory

### Option 2: Using ImageMagick (command line)
```bash
# Create simple placeholder icons with text
convert -size 192x192 xc:#0f3460 -gravity center \
        -pointsize 72 -fill white -annotate +0+0 "ESP32" \
        icon-192.png

convert -size 512x512 xc:#0f3460 -gravity center \
        -pointsize 200 -fill white -annotate +0+0 "ESP32" \
        icon-512.png
```

### Option 3: Using GIMP or Photoshop
1. Create a 512x512 canvas
2. Design your icon (simple "ESP32" text or gateway symbol)
3. Use background color: #0f3460 (dark blue)
4. Use foreground color: #ffffff (white)
5. Export as PNG at 512x512
6. Resize to 192x192 for the smaller version

## Simple Temporary Icons

For testing, you can use solid color squares:

```bash
# Solid blue square 192x192
convert -size 192x192 xc:#3498db icon-192.png

# Solid blue square 512x512
convert -size 512x512 xc:#3498db icon-512.png
```

## Placement
Place both icon files in:
```
D:\Programing\esp-idf\projects\AiAgent\CLCode01\web\
```

## Verification
After adding icons, rebuild the SPIFFS image:
```bash
idf.py build
```

The icons will be automatically embedded in the SPIFFS partition.
