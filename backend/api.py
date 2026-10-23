"""Flask API for household configuration and controller state."""

import logging
import os
from datetime import datetime, timezone
from ipaddress import ip_address, ip_network
from pathlib import Path
from urllib.parse import urlsplit
from zoneinfo import ZoneInfo

from flask import Flask, jsonify, request
from werkzeug.exceptions import HTTPException

from . import model
from .controller_api import register_controller_routes
from .controllers import OperationStore, validate_reservations
from .scheduler import active_target
from .storage import HouseStore

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_NETWORKS = "127.0.0.0/8,10.0.0.0/8,172.16.0.0/12,192.168.0.0/16,::1/128,fc00::/7,fe80::/10"


def system_timezone():
    if os.getenv("TZ"):
        return os.environ["TZ"]
    try:
        name = Path("/etc/timezone").read_text().strip()
        ZoneInfo(name)
        return name
    except (OSError, ValueError):
        pass
    try:
        localtime = str(Path("/etc/localtime").resolve())
        if "/zoneinfo/" in localtime:
            return localtime.split("/zoneinfo/", 1)[1]
    except OSError:
        pass
    return "UTC"


def create_app(config=None):
    app = Flask(__name__)
    app.config.update(
        DATA_DIR=os.getenv("BMS_DATA_DIR", str(ROOT / "data" / "houses")),
        DEFAULT_TIMEZONE=os.getenv("BMS_TIMEZONE", system_timezone()),
        ALLOWED_NETWORKS=os.getenv("BMS_ALLOWED_NETWORKS", DEFAULT_NETWORKS),
        ALLOWED_ORIGINS=os.getenv("BMS_ALLOWED_ORIGINS", "http://BMS:3000,http://bms.local:3000,http://localhost:3000,http://127.0.0.1:3000"),
        MAX_CONTENT_LENGTH=1_048_576,
        NOW=lambda: datetime.now(timezone.utc),
    )
    app.config.update(config or {})
    ZoneInfo(app.config["DEFAULT_TIMEZONE"])
    store = HouseStore(app.config["DATA_DIR"], app.config["DEFAULT_TIMEZONE"])
    app.extensions["house_store"] = store
    operations = OperationStore(app.config.get("CONTROLLER_DATA_DIR") or Path(app.config["DATA_DIR"]).parent / "controllers")
    app.extensions["controller_operations"] = operations
    networks = [ip_network(n.strip()) for n in app.config["ALLOWED_NETWORKS"].split(",") if n.strip()]
    origins = {o.strip().lower() for o in app.config["ALLOWED_ORIGINS"].split(",") if o.strip()}

    def local_address(value):
        try:
            address = ip_address(value)
            return any(address in network for network in networks)
        except ValueError:
            return False

    def allowed_origin(value):
        if value.lower() in origins:
            return True
        try:
            parts = urlsplit(value)
            return parts.scheme == "http" and parts.port == 3000 and not parts.username and not parts.password and not parts.path and local_address(parts.hostname or "")
        except ValueError:
            return False

    @app.before_request
    def local_only():
        # Use the direct connection address. Never trust X-Forwarded-For.
        if not local_address(request.remote_addr or ""):
            raise model.Problem("This API is available only on the configured local network.", 403, "local_only")
        origin = request.headers.get("Origin")
        if origin and not allowed_origin(origin):
            raise model.Problem("This frontend origin is not allowed.", 403, "origin_not_allowed")

    @app.after_request
    def headers(response):
        response.headers["Cache-Control"] = "no-store"
        response.headers["X-Content-Type-Options"] = "nosniff"
        origin = request.headers.get("Origin")
        if origin and allowed_origin(origin):
            response.headers["Access-Control-Allow-Origin"] = origin
            response.headers.add("Vary", "Origin")
            response.headers["Access-Control-Allow-Methods"] = "GET,PUT,POST,PATCH,DELETE,OPTIONS"
            response.headers["Access-Control-Allow-Headers"] = "Content-Type"
        return response

    @app.errorhandler(model.Problem)
    def problem(error):
        if error.status >= 500:
            app.logger.error("%s", error, exc_info=True)
        return jsonify(error={"code": error.code, "message": str(error)}), error.status

    @app.errorhandler(HTTPException)
    def http_problem(error):
        return jsonify(error={"code": error.name.lower().replace(" ", "_"), "message": error.description}), error.code

    @app.errorhandler(Exception)
    def unexpected(error):
        app.logger.exception("Unexpected request failure")
        return jsonify(error={"code": "server_error", "message": "The request could not be completed."}), 500

    def body():
        if not request.is_json:
            raise model.Problem("Send a JSON body with Content-Type application/json.")
        value = request.get_json()
        if not isinstance(value, dict):
            raise model.Problem("The request body must be a JSON object.")
        return value

    def requested_id():
        return model.text(request.args.get("id"), "Household ID")

    def get_house(houses, identifier):
        if identifier not in houses:
            raise model.Problem("Household was not found.", 404, "not_found")
        return houses[identifier]

    def save(houses, value):
        valid = model.household(value["id"], value, app.config["DEFAULT_TIMEZONE"])
        houses[valid["id"]] = valid
        model.validate_assignments(houses)
        with operations.transaction() as pending:
            validate_reservations(houses, pending)
            store.save(valid)
        return valid

    @app.get("/get_house_list")
    def house_list():
        with store.transaction() as houses:
            return jsonify([{"id": h["id"], "name": h["name"]} for h in sorted(houses.values(), key=lambda h: (h["name"].casefold(), h["id"]))])

    @app.get("/get_config")
    def get_config():
        identifier = requested_id()
        with store.transaction() as houses:
            return jsonify(get_house(houses, identifier))

    @app.put("/set_config")
    def set_config():
        identifier, raw = requested_id(), body()
        value = model.household(identifier, raw, app.config["DEFAULT_TIMEZONE"])
        with store.transaction() as houses:
            created = identifier not in houses
            value = save(houses, value)
            return jsonify(value), 201 if created else 200

    @app.get("/get_state")
    def get_state():
        with store.transaction() as houses:
            for house in houses.values():
                for room in house["rooms"]:
                    if room["controller_ip"] == request.remote_addr:
                        return jsonify(
                            target_temperature=active_target(room, app.config["NOW"]()),
                            negative_delta=room["negative_delta"], positive_delta=room["positive_delta"], sensor_mac=room["sensor_mac"],
                        )
        raise model.Problem("Controller IP is not assigned to a room.", 404, "unknown_controller")

    @app.post("/households/<house_id>/rooms")
    def add_room(house_id):
        value = model.room(body(), require_programs=False)
        with store.transaction() as houses:
            house = get_house(houses, house_id)
            model.add_unique(house["rooms"], value, "Room")
            saved = save(houses, house)
            return jsonify(model.find(saved["rooms"], value["id"], "Room")), 201

    @app.route("/households/<house_id>/rooms/<room_id>", methods=["PATCH", "DELETE"])
    def edit_room(house_id, room_id):
        changes = model.patch(body(), model.ROOM_FIELDS) if request.method == "PATCH" else None
        with store.transaction() as houses:
            house = get_house(houses, house_id)
            room = model.find(house["rooms"], room_id, "Room")
            if request.method == "DELETE":
                house["rooms"].remove(room)
                save(houses, house)
                return "", 204
            room.update(changes)
            saved = save(houses, house)
            return jsonify(model.find(saved["rooms"], room_id, "Room"))

    @app.post("/households/<house_id>/rooms/<room_id>/programs")
    def add_program(house_id, room_id):
        value = model.program(body())
        with store.transaction() as houses:
            house = get_house(houses, house_id)
            room = model.find(house["rooms"], room_id, "Room")
            model.add_unique(room["programs"], value, "Program")
            saved = save(houses, house)
            return jsonify(model.find(model.find(saved["rooms"], room_id, "Room")["programs"], value["id"], "Program")), 201

    @app.route("/households/<house_id>/rooms/<room_id>/programs/<program_id>", methods=["PATCH", "DELETE"])
    def edit_program(house_id, room_id, program_id):
        changes = model.patch(body(), model.PROGRAM_FIELDS) if request.method == "PATCH" else None
        with store.transaction() as houses:
            house = get_house(houses, house_id)
            room = model.find(house["rooms"], room_id, "Room")
            program = model.find(room["programs"], program_id, "Program")
            if request.method == "DELETE":
                room["programs"].remove(program)
                save(houses, house)
                return "", 204
            program.update(changes)
            saved = save(houses, house)
            result = model.find(model.find(saved["rooms"], room_id, "Room")["programs"], program_id, "Program")
            return jsonify(result)

    register_controller_routes(app, store, operations)
    return app
