// Build against the same Bunker server module as the APK. The desktop Archive
// calls these converters in place of the phone's Java NativeBridge.
package main

import (
	"fmt"
	"os"
	"strconv"

	"lunar-tear/server/mobile"
)

func main() {
	args := os.Args[1:]
	if len(args) == 2 && args[0] == "verify-resources" {
		if err := checkResourceArchive(args[1]); err != nil {
			fmt.Fprint(os.Stderr, err)
			os.Exit(1)
		}
		return
	}
	message := "Invalid Archive converter arguments"
	if len(args) >= 3 {
		switch args[0] {
		case "decode":
			if len(args) == 4 {
				if size, err := strconv.Atoi(args[3]); err == nil {
					message = mobile.Texture(args[1], args[2], size)
				}
			}
		case "sound":
			message = mobile.Audio(args[1], args[2])
		case "model":
			message = mobile.Model(args[1], args[2])
		case "motion":
			if len(args) == 4 {
				message = mobile.Motion(args[1], args[2], args[3])
			}
		case "scenario":
			message = mobile.Scenario(args[1], args[2])
		}
	}
	if message != "" {
		fmt.Fprint(os.Stderr, message)
		os.Exit(1)
	}
}
