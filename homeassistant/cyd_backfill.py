# config/pyscript/cyd_backfill.py
#
# Generic history backfill service for all CYD graphs.
#
# A graph with "backfill": true and an "entity" asks for history by publishing to
#     home/history/get/<entity-path>        e.g. home/history/get/sensor/power
# optionally with a payload {"points":120,"interval":120}.
# This service reads the entity from the topic (sensor/power -> sensor.power),
# fetches the recent history from Home Assistant's recorder, and publishes the
# values back (oldest first, JSON array) to:
#     home/history/<entity-path>             e.g. home/history/sensor/power
#
# One handler serves every graph; add graphs freely, no edits here.
#
# Requires the "pyscript" integration (HACS) with, in configuration.yaml:
#
#   pyscript:
#     allow_all_imports: true
#     hass_is_global: true
#
# After editing, reload pyscript (Developer Tools -> Actions -> pyscript.reload).

import json
import datetime
from homeassistant.util import dt as dt_util
from homeassistant.components.recorder.history import get_significant_states

REQUEST_PREFIX = "home/history/get/"   # device publishes here
REPLY_PREFIX   = "home/history/"       # service replies here
ALLOWED_DOMAINS = ("sensor",)          # only answer for these entity domains


@mqtt_trigger(REQUEST_PREFIX + "#")
def cyd_backfill(topic=None, payload=None, **kwargs):
    # topic -> entity: home/history/get/sensor/power  ->  sensor.power
    path = topic[len(REQUEST_PREFIX):]
    entity = path.replace("/", ".")

    # Safety: only serve the domains we expect.
    if "." not in entity or entity.split(".", 1)[0] not in ALLOWED_DOMAINS:
        log.warning(f"CYD backfill: ignoring request for '{entity}'")
        return

    # Request options {"points":120,"interval":120}
    try:
        req = json.loads(payload) if payload else {}
    except Exception:
        req = {}
    points = int(req.get("points", 120))
    interval = int(req.get("interval", 120))

    end = dt_util.utcnow()
    start = end - datetime.timedelta(seconds=points * interval)

    # History is a blocking DB read -> run it in the executor thread.
    states = task.executor(get_significant_states, hass, start, end, [entity])
    rows = states.get(entity, []) if states else []

    # Keep only numeric values, oldest first.
    vals = []
    for st in rows:
        try:
            vals.append(round(float(st.state), 1))
        except (ValueError, TypeError):
            pass  # skip "unavailable" / "unknown"

    # Downsample to roughly `points` values.
    if len(vals) > points:
        step = len(vals) // points
        vals = vals[::step][-points:]

    reply_topic = REPLY_PREFIX + path
    mqtt.publish(topic=reply_topic, payload=json.dumps(vals), qos=0, retain=False)
    log.info(f"CYD backfill: {entity} -> {len(vals)} points on {reply_topic}")
