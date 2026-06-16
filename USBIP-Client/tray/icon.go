package tray

import (
	"bytes"
	"image"
	"image/color"
	"image/png"
	"math"
)

// iconBytes returns a generated 32×32 USB icon as PNG bytes.
// getlantern/systray accepts PNG on all platforms.
func iconBytes() []byte {
	const size = 32
	img := image.NewRGBA(image.Rect(0, 0, size, size))
	cx, cy := float64(size)/2-0.5, float64(size)/2-0.5
	blue := color.RGBA{0x00, 0x78, 0xD4, 0xFF}
	white := color.RGBA{0xFF, 0xFF, 0xFF, 0xFF}

	for y := 0; y < size; y++ {
		for x := 0; x < size; x++ {
			dx, dy := float64(x)-cx, float64(y)-cy
			if math.Sqrt(dx*dx+dy*dy) <= 15.5 {
				img.Set(x, y, blue)
			}
		}
	}
	// USB plug symbol
	fill(img, 15, 7, 17, 21, white)  // stem
	fill(img, 11, 7, 21, 9, white)   // top bar
	fill(img, 11, 11, 13, 15, white) // left prong
	fill(img, 19, 11, 21, 15, white) // right prong
	fill(img, 13, 20, 19, 25, white) // connector

	var buf bytes.Buffer
	png.Encode(&buf, img)
	return buf.Bytes()
}

func fill(img *image.RGBA, x0, y0, x1, y1 int, c color.RGBA) {
	for y := y0; y < y1; y++ {
		for x := x0; x < x1; x++ {
			img.Set(x, y, c)
		}
	}
}
