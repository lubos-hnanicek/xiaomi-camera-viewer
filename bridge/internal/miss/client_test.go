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

		// A published CW300 go2rtc setup works without a subtype override;
		// upstream's default is profile 2. Both regional ids keep that profile
		// until a hardware sweep can replace the external evidence.
		{"cw300 china hd", ModelMoc001, "hd", "2"},
		{"cw300 china default", ModelMoc001, "", "2"},
		{"cw300 global hd", ModelMoc006, "hd", "2"},
		{"cw300 global default", ModelMoc006, "", "2"},

		// No hardware profile sweep has been published for the CW700S. Keep the
		// ordinary MISS default until scripts/probe-quality.ps1 can measure one.
		{"cw700s hd", Model700SA, "hd", "2"},
		{"cw700s default", Model700SA, "", "2"},

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

func TestMediaStartBody(t *testing.T) {
	tests := []struct {
		name      string
		channel   string
		primary   string
		secondary string
		audio     bool
		want      string
	}{
		{
			name:    "primary",
			channel: "",
			primary: "hd",
			audio:   true,
			want:    `{"videoquality":3,"enableaudio":1}`,
		},
		{
			name:    "secondary",
			channel: "1",
			primary: "sd",
			want:    `{"videoquality":-1,"videoquality2":1,"enableaudio":0}`,
		},
		{
			name:      "both",
			channel:   "both",
			primary:   "hd",
			secondary: "sd",
			audio:     true,
			want:      `{"videoquality":3,"videoquality2":1,"enableaudio":1}`,
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			got := mediaStartBody(Model500DH, tt.channel, tt.primary, tt.secondary, tt.audio)
			if got != tt.want {
				t.Errorf("mediaStartBody() = %q, want %q", got, tt.want)
			}
		})
	}
}
