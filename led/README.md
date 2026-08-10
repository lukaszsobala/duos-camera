# Milk-V Duo S: turning the LEDs off

Not camera work - it just lives here because this is the scratch directory for
Duo S bits that are not wired into the Armbian build yet.

## The short answer

There are two LEDs and only one of them is yours:

| LED  | What it is | Controllable |
|------|------------|--------------|
| blue | user LED on `XGPIOA[29]`, pad `IIC0_SDA` function 3, driven through a MOSFET | yes |
| red  | power indicator, wired to the 3.3V rail | **no** - it is on whenever the board has power |

So "turn the LEDs off" means "turn the blue one off". The red one has no GPIO
behind it; if you want it dark, the only options are physical.

## The command

Under Armbian nothing claims `XGPIOA[29]`, so it is not in `/sys/class/leds`
and there is nothing to `echo 0` into. It has to be driven as a raw GPIO. The
sysfs number is *not* Milk-V's 509 - that is their 5.10 kernel's numbering, and
mainline allocates gpiochip bases differently - so it has to be looked up:

    base=$(cat /sys/class/gpio/gpiochip*/base | head -1)   # NO, see below

That is the trap: there are four banks and the bases are dynamic. Find the one
that is `gpio@3020000` (XGPIOA):

    for c in /sys/class/gpio/gpiochip*; do
        grep -q 3020000 "$c/label" 2>/dev/null && base=$(cat "$c/base")
    done
    echo $((base + 29)) > /sys/class/gpio/export
    echo low > /sys/class/gpio/gpio$((base + 29))/direction

`echo low` sets direction and level in one write, so the pin does not glitch
high on its way to becoming an output.

## The script

`duos-led.sh` does the above, as root:

    ./duos-led.sh            # off (the default)
    ./duos-led.sh on
    ./duos-led.sh status

It finds the bank by name rather than assuming a base, and if the overlay below
is applied it notices the LED class device and uses that instead - because once
`leds-gpio` claims the line, `/sys/class/gpio/export` returns EBUSY.

This lasts until reboot. For a permanent setting, either call the script from
`/etc/rc.local`, or use the overlay.

If driving the pin low turns the LED *on* rather than off, the wiring is the
other way round from what is assumed here; re-run with `LED_ACTIVE_LOW=1` and
tell me, because then the overlay needs `GPIO_ACTIVE_LOW` too.

## The overlay (persistent, and the better answer)

`sg2000-duos-led.dtso` adds a `gpio-leds` node with `default-state = "off"`, so
the LED is off from boot and afterwards appears as an ordinary LED class device
at `/sys/class/leds/blue:status/`. `CONFIG_LEDS_GPIO=y` and
`CONFIG_LEDS_CLASS=y` are already set in all four Duo S kernel configs, so no
config change is needed.

To use it, copy it into the kernel's overlay directory alongside the existing
`sg2000-*.dtso` files, add it to `arch/<arch>/boot/dts/sophgo/Makefile` the way
the others are, rebuild, and put

    overlays=duos-led

in `/boot/armbianEnv.txt`. Properly it should become an Armbian patch in
`patch/kernel/archive/sophgo-sg200x-7.2/` like the other overlays - I have not
done that, since nothing here is wired into the build yet.

Once it is in place you get triggers for free, which is probably what you
actually want long term:

    echo heartbeat > /sys/class/leds/blue:status/trigger   # blink with load
    echo mmc0      > /sys/class/leds/blue:status/trigger   # SD activity
    echo none      > /sys/class/leds/blue:status/trigger   # manual
    echo 0         > /sys/class/leds/blue:status/brightness

## State

- The pin assignment is from Milk-V's own `cvi_board_init.c`
  (`PINMUX_CONFIG(IIC0_SDA, XGPIOA_29)`, in
  `build/boards/cv181x/sg2000_milkv_duos_*/u-boot/`), cross-checked against
  their `blink.sh` (`LED_PIN=509`, which is bank A base 480 + 29 in their
  numbering) and Milk-V's Duo S page, which documents the blue LED as
  "XGPIOA[29], SG2000 number 509".
- Mux function 3 is from the vendor pinlist header
  (`IIC0_SDA__XGPIOA_29 3`).
- The overlay compiles with `dtc` and applies to `sg2000-milkv-duo-s.dtb` with
  `fdtoverlay`; `&porta` and `&pinctrl` both resolve. The script passes
  `sh -n`.
- **Neither has been run on hardware.** In particular the active-high
  assumption is inference - the pad is a default-pull-up I2C pad left as an
  input, which is the obvious explanation for the LED being lit on a stock
  image, but it has not been confirmed with a meter or an eyeball.
