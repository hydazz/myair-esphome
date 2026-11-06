# myair-esphome

ESPHome component that replaces the MyAir 5 touch screen. The ESP plugs into the control box's touch screen port, the RS-485 bus the zone dampers and sensors hang off, and runs the system: mode, fan, the reference zone and every zone setpoint. Home Assistant gets one climate per room and one for the AC.

The control box keeps its own jobs: the damper loop, the RF temperature sensors and the link to the AC, whose protections stay in charge.

## Limitations

- It is written for a CB10 control box behind a MyAir 5 touch screen. Other control boxes that speak the same protocol may work; none has been tried.
- One master on the bus. Unplug the touch screen before the ESP goes on, and never run both.
- It has run against a simulated control box, not yet a live one. The cable's pinout and voltage are unconfirmed: community sources put RS-485 on pins 1 and 2 and about 14 V on pin 5, and one owner disagrees. Measure first.
- Untested on hardware: zones without sensors, constant zones, defrost, and what the control box sends the AC with no temperature zone on. The reported heating and cooling damper curves have been observed with the stock controller; the replacement has not run either on hardware.
- Thresholds and holds are starting values from one house. Airflow weights and damper percentages are estimates, not measured flow or pressure. There is no validated minimum airflow, and the fan ceiling applies only to Fan Auto.
- The control box repeats cached temperatures between RF reports. In an attic probe battery test, it reported the cached temperature for ten minutes after the last RF report, then marked the sensor absent. The ESP ignores the temperature once the box marks the sensor absent.
- Motion sensors aren't read; presence belongs to Home Assistant.

## Hardware

Any ESPHome board with a hardware UART and a 3.3 V RS-485 transceiver. The YAML below is for an ATOMS3 Lite on an A131 Atomic RS485 base, which switches direction itself. The bus runs at 57600 baud, 8N1.

## Configuration

```yaml
external_components:
  - source: github://hydazz/myair-esphome@main
    components: [myair]

uart:
  id: cb_uart
  tx_pin: GPIO6
  rx_pin: GPIO5
  baud_rate: 57600
  rx_buffer_size: 2048

myair:
  uart_id: cb_uart
  zones:
    - zone: 1
      name: Living Room
      sensor_uid: "011210"
      airflow_weight: 2

    - zone: 7
      name: Office

    - zone: 9
      name: Garage
      sensor: false
```

Each zone takes:

- **zone** (required, 1–10): the control box's physical output. The ESP controls every listed zone and leaves an unlisted one as the control box has it.
- **name** (optional): defaults to the zone's device name, or Zone _n_.
- **device_id** (optional): an ESPHome sub-device for the zone; see below.
- **sensor** (optional, default true): `false` makes an airflow zone, set to a damper position in 5 % steps, as the touch screen does. A zone with a sensor is a room with a range.
- **sensor_uid** (optional, six hex digits, quoted): the RF sensor to assign.
- **airflow_weight** (optional, default 1): an estimate of the zone's airflow with its damper fully open, relative to the other zones'. It weighs the zone in Fan Auto, in Auto's choice of side and in whether the zone can carry the AC. Duct cross-section is a starting estimate: two independent feeds the size of another zone's one give 2; two outlets split from one feed still share that feed. A 6" duct has about half an 8" duct's area. Leave the installer's balancing out: the ESP reads each zone's maximum damper and scales the weight by it. This linear approximation does not account for duct resistance or measure room heat load. Room area or volume is not an airflow measurement.
- Entity keys, below.

### A device per room

A zone can be its own ESPHome sub-device, which Home Assistant shows as a device in that room's area. The zone then takes its name from the device:

```yaml
esphome:
  name: myair
  areas:
    - id: living_room
      name: Living Room
  devices:
    - id: living_room_zone
      name: Living Room Zone
      area_id: living_room

myair:
  uart_id: cb_uart
  zones:
    - zone: 1
      device_id: living_room_zone
```

## Entities

System, on the main device:

| Key              | Entity      | Default | Shows or sets                                                 |
| ---------------- | ----------- | ------- | ------------------------------------------------------------- |
| `climate`        | Climate     | yes     | The AC: mode, fan, and the temperature and target it works to |
| `reference_zone` | Text sensor | no      | The zone the AC regulates on                                  |
| `ac_mode`        | Text sensor | no      | The AC's running mode, or Off                                 |
| `ac_fan`         | Text sensor | no      | The AC's running fan speed, or Off                            |
| `connected`      | Binary      | no      | The control box has answered and sent its snapshot            |

Each zone:

| Key                  | Entity      | Temperature zone | Airflow zone |
| -------------------- | ----------- | ---------------- | ------------ |
| `climate`            | Climate     | yes              | –            |
| `cover`              | Cover       | –                | yes          |
| `temperature`        | Sensor      | yes              | –            |
| `damper`             | Sensor      | yes              | no           |
| `min_damper`         | Number      | no               | no           |
| `max_damper`         | Number      | no               | no           |
| `temperature_offset` | Number      | no               | –            |
| `pair_sensor`        | Switch      | no               | –            |
| `unpair_sensor`      | Button      | no               | –            |
| `sensor_id`          | Text sensor | no               | –            |

Yes is created by default, no on request, – not at all. Set an entity to a name, a mapping of its ESPHome options, `{}` for an optional entity with its default name, or `false` to omit it. Mappings accept the usual options for the domain, such as `id`, `filters`, `icon` and `visual`. Omitting an entity doesn't change what the ESP controls: the zone keeps its saved state.

- **Cover:** an airflow zone's damper. Open turns the zone on at its last position, a position sets it in 5 % steps, and closed turns it off.
- **Damper:** the position the control box's loop sets, as a share of the zone's range: 5 % at or inside its setpoint, 30 % plus 70 % per °C past it in 5 % steps, 100 % from 1 °C past and for the reference zone. A closed zone reads 0. It is a commanded position, not measured airflow.
- **Minimum Damper** and **Maximum Damper:** the control box's balancing limits in 5 % steps, the settings on the touch screen's technician balancing page. They are optional because they rewrite the installer's balancing.
- **Temperature Offset:** calibrates a zone's sensor between −5 and +5 °C. The ESP subtracts it from the setpoint it sends, so the room still targets its range.

## How the ESP runs the AC

The AC's climate sets the mode (Off, Auto, which Home Assistant shows as Heat/Cool, Cool, Heat, Fan only, Dry) and fan (Auto, Low, Medium, High). With temperature rooms configured, it shows a heat-to/cool-to range in Auto, the heating target in Heat, and the cooling target in Cool. These are the reference room's targets, or the first configured room's before a reference is selected. Its current temperature is the reference room's temperature.

Changing a main target applies that edge to every temperature room, including rooms that are off. Setting a main range applies both edges. This is a one-time edit: a subsequent room edit stays in that room, and switching modes does not reset it. Room on/off states and airflow zones are unchanged. Each room keeps its range at least 2 °C wide. Ranges can be prepared while the AC is off; Fan only and Dry have no temperature dial in Home Assistant.

Without any temperature rooms configured, the main climate keeps a single target for the AC's own thermostat. While airflow zones run without an eligible temperature room, the AC uses that saved single target and its own Auto mode; room ranges cannot regulate return air whose temperature is absent from the bus.

A room's climate is Off or Heat/Cool, with a heat-to and a cool-to temperature in whole degrees, at least 2 °C apart. Set one edge closer and the other moves away; at 16 or 32 °C, the edge you set gives way instead. The gap puts Auto's changeover points 4 °C apart, so Auto doesn't swing between heating and cooling. Home Assistant turns rooms on and off and moves their ranges; the ESP turns that into control box settings. A room calls once it is more than 0.5 °C past the edge for the current side.

Mode and room targets are sent together without cycling power. Their readback is published together, so climate actions never combine the old mode with new targets. An idle system stays idle across Heat/Cool changes when neither side needs conditioning. Climate actions are estimates from the control box's mode, temperatures and setpoints; the bus does not report compressor activity.

- **Rooms.** While the AC runs, an on room's zone is open and gets its cool-to (cooling, drying) or heat-to (heating) as its zone setpoint, and the control box modulates its damper against it. An off room's zone closes once it is no longer the reference.
- **Airflow zones** hold their requested damper position while the AC runs.
- **Power follows the zones.** The AC runs while the mode isn't Off and a room that can carry it with a readable temperature, or an airflow zone, is on. Turning the last one off requests AC Off. Shutdown leaves the reference and all damper commands untouched, including constant zones: a power-off report does not prove the indoor fan has stopped. Room and airflow requests are saved while off and applied at the next start. An invalid temperature disqualifies a room as the reference; if no eligible room or airflow zone remains, the AC stops.
- **Reference zone.** The AC regulates on one zone and backs off once that room is satisfied, and the control box holds that zone's damper at its maximum. The ESP starts it in the open room furthest past its edge, the larger airflow on a tie. Once the reference room is satisfied and another room is more than 0.3 °C past its edge, the reference moves to the room furthest past its edge, at most every 10 minutes, so the AC keeps working for the rooms that need it. It moves at once if the reference room turns off. When the AC stops, the reference stays where it was, so that zone stays open.
- **Small rooms.** A zone with at most 15 % of the largest zone's airflow at its maximum damper, one the installer throttled down, can't carry the AC. It never takes the reference, runs the AC on its own or chooses Auto's side; it gets air while a larger room runs the AC.
- **Trims.** The control box's damper loop is proportional, so a room with more load than its share of air settles past its edge with its damper part open. While another open room is more than 0.3 °C past its edge, the ESP sets its zone setpoint 1 °C past the edge, so its damper opens fully sooner, and takes the trim off once the room is 0.2 °C back inside. A trim changes at most every 10 minutes.
- **Auto** cools or heats. It starts on the side the AC is running while a room calls on it or no room is 1 °C past its other edge, and otherwise on the side with more demand weighted by each room's airflow. It changes side only after no room has called on the current side for 15 minutes and a room is 1 °C past its other edge.
- **Fan Auto** follows the airflow the open zones ask for: each room's airflow at its maximum damper, scaled by how far it is past its edge up to 2 °C, and each airflow zone's at its position. Medium starts at 20 % of the system's airflow and high at 40 %, and they hold down to 10 % and 30 %. The control box opens a damper fully at 1 °C past, and beyond that only more air helps, so a candidate for the reference also asks for medium on its own at 1.2 °C past its edge and high at 2.2 °C, holding them down to 0.8 and 1.8 °C. The fan moves one speed at a time, up at most every 10 minutes and down every 20.
- **The fan's ceiling.** Fan Auto caps its requested speed using the damper positions this batch leaves open: medium needs 10 % of the system's estimated airflow open and high 40 %, and staying on a speed needs three quarters of that. Below that the next fan write drops without a hold. These are starting thresholds, not measured airflow or a guarantee of acceptable duct pressure. A fan speed set by hand stands.
- **The AC's own setpoint.** The ESP writes the reference room's edge as the AC's own setpoint too, for a control box wired to send the AC return air. With no temperature zone on, it writes reference 0 and the AC's own setpoint, the AC's last target, and Auto becomes the AC's own auto. The control box's installation manual has DIP switch 5 select a return-air sensor.
- **Constant zones.** An installer's constant zones, listed in the control box, open while the other open zones' damper positions add up to less than 100 % per constant zone, as the touch screen does. List a constant zone for the ESP to manage it.

## Temperature sensors

The sensor UID is printed inside the case, beside the QR code. Set `sensor_uid` on the zone to assign it, quoted.

To pair from Home Assistant, enable `pair_sensor`, `unpair_sensor` and `sensor_id` on the zone, then:

1. Turn on the zone's **Pair Sensor** switch.
2. Hold the button inside the probe for six seconds and release it. Its LED flashes.
3. **Sensor UID** shows the UID once the control box receives the probe.

Pairing stops on success, failure or after 60 seconds. **Unpair Sensor** removes the assignment, and pairing a probe to another zone moves it. Pairing and unpairing save the assignment on the ESP, which then takes precedence over `sensor_uid`. Assignments go to the control box on every reconnect.

## What the ESP saves

On change, to flash: the mode and fan, the AC's own setpoint, each room's on state and range, each airflow zone's on state and position, sensor offsets and the sensor assignments pairing makes. On first boot every zone is off and the mode is Off. Damper limits and constant zones live in the control box.

## Development

```sh
mise install
mise run deps
mise run fmt-check
mise run lint
mise run test
mise run build
```

Protocol references: [cb-daemon](https://github.com/retsimx/cb-daemon) and
[aa_interop](https://github.com/gundy/aa_interop).
