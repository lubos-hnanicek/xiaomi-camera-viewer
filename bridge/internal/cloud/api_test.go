package cloud

import (
	"encoding/json"
	"testing"
)

func TestSharedHomesResponsePreservesNumericIDs(t *testing.T) {
	var response sharedHomesResponse
	err := json.Unmarshal([]byte(`{
		"share": {
			"share_family": [
				{"home_id": 123456789012345, "home_owner": 987654321098765}
			]
		}
	}`), &response)
	if err != nil {
		t.Fatal(err)
	}

	homes := response.Share.ShareFamily
	if len(homes) != 1 {
		t.Fatalf("got %d shared homes, want 1", len(homes))
	}
	if homes[0].HomeID != "123456789012345" || homes[0].HomeOwner != "987654321098765" {
		t.Fatalf("shared home IDs = (%s, %s)", homes[0].HomeID, homes[0].HomeOwner)
	}
}

func TestSharedHomeDevicesResponse(t *testing.T) {
	var response sharedHomeDevicesResponse
	err := json.Unmarshal([]byte(`{
		"device_info": [
			{
				"did": "shared-camera",
				"name": "Shared camera",
				"model": "xiaomi.camera.test",
				"mac": "00:11:22:33:44:55",
				"localip": "192.168.1.20"
			}
		]
	}`), &response)
	if err != nil {
		t.Fatal(err)
	}

	if len(response.DeviceInfo) != 1 {
		t.Fatalf("got %d shared devices, want 1", len(response.DeviceInfo))
	}
	device := response.DeviceInfo[0]
	if device.Did != "shared-camera" || device.Model != "xiaomi.camera.test" ||
		device.MAC != "00:11:22:33:44:55" || device.IP != "192.168.1.20" {
		t.Fatalf("unexpected shared device: %+v", device)
	}
}

func TestCameraDevicesMergesSharedAndOwnedWithoutDuplicates(t *testing.T) {
	owned := &Device{Did: "owned", Model: "xiaomi.camera.indoor"}
	shared := &Device{Did: "shared", Model: "xiaomi.cateye.doorbell"}
	duplicate := &Device{Did: "owned", Model: "xiaomi.camera.indoor"}
	nonCamera := &Device{Did: "plug", Model: "xiaomi.plug.v1"}

	got := cameraDevices(
		[]*Device{owned, nonCamera},
		[]*Device{shared, duplicate, nil},
	)
	if len(got) != 2 {
		t.Fatalf("got %d cameras, want 2: %+v", len(got), got)
	}
	if got[0] != owned || got[1] != shared {
		t.Fatalf("camera order/results = %+v, want owned then shared", got)
	}
}
