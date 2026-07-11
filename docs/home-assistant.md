# Home Assistant setup

This integration publishes weather facts directly from Home Assistant. It does
not call an LLM and cannot update the assistant note.

## 1. Enable packages

In `configuration.yaml`:

```yaml
homeassistant:
  packages: !include_dir_named packages
```

Create the `packages` directory if it does not exist, then copy:

```text
integrations/home-assistant/papercolor_pulse.yaml
```

to:

```text
config/packages/papercolor_pulse.yaml
```

## 2. Select the weather entity

Open Developer Tools -> States and find the weather entity you already use.
Replace both occurrences of `weather.home` in the package with that entity ID.

The entity must support `weather.get_forecasts` with a daily forecast containing
numeric `templow` and `temperature` fields.

## 3. Add secrets

In `secrets.yaml`:

```yaml
papercolor_gateway_ha_source_url: http://GATEWAY_LAN_IP:8767/admin/v1/pulse/sources/ha
papercolor_gateway_admin_authorization: Bearer YOUR_ADMIN_TOKEN
```

Keep the bearer in `secrets.yaml`; do not paste it into the package committed to
Git.

## 4. Validate and restart

Use Developer Tools -> YAML -> Check configuration, then restart Home
Assistant. Run automation `PaperColor Publish Weather Facts` manually once.

## 5. Verify

The gateway access log should show:

```text
PUT /admin/v1/pulse/sources/ha 200
```

The next device synchronization should fetch a new Pulse generation. If the
automation is skipped, check that the weather entity is available and the
daily low/high values are numeric.
