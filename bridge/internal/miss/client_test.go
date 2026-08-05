package miss

import "testing"

func TestResolveQuality(t *testing.T) {
	tests := []struct {
		name    string
		model   string
		quality string
		want    string
	}{
		{"default is hd", "chuangmi.camera.ipc019", "", "2"},
		{"explicit hd", "chuangmi.camera.ipc019", "hd", "2"},
		{"sd", "chuangmi.camera.ipc019", "sd", "1"},
		{"auto", "chuangmi.camera.ipc019", "auto", "0"},

		// Measured: quality 2 sends nothing on the CW400 and only the 640x360
		// substream on the CW500, so hd must resolve to 3 for both.
		{"cw400 hd", ModelHLC8, "hd", "3"},
		{"cw400a hd", ModelHLC8A, "hd", "3"},
		{"cw400a default", ModelHLC8A, "", "3"},
		{"cw500 hd", Model500DH, "hd", "3"},
		{"cw500 default", Model500DH, "", "3"},
		{"mod11 hd", ModelMod11, "hd", "3"},
		{"c200 hd", ModelC200, "hd", "3"},
		{"c300 hd", ModelC300, "hd", "3"},

		// sd and auto are not model specific, even for the odd models.
		{"cw400a sd", ModelHLC8A, "sd", "1"},
		{"cw400a auto", ModelHLC8A, "auto", "0"},
		{"cw500 sd", Model500DH, "sd", "1"},

		// A numeric profile is an escape hatch and must pass through as given.
		{"numeric passthrough", ModelHLC8A, "5", "5"},
		{"numeric passthrough on known model", Model500DH, "4", "4"},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			if got := resolveQuality(tt.model, tt.quality); got != tt.want {
				t.Errorf("resolveQuality(%q, %q) = %q, want %q",
					tt.model, tt.quality, got, tt.want)
			}
		})
	}
}
