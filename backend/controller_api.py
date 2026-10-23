"""Room controller management through direct, identity-checked local HTTP."""

import copy
import uuid
from ipaddress import IPv4Address, ip_network

from flask import jsonify, request

from . import model
from .controllers import (
    SETUP_IP, CONFIG_FIELDS, ControllerFailure, ControllerTransport, clean_status,
    config_patch, operation_key, public_operation, validate_reservations, verified,
)


def register_controller_routes(app, houses_store, operations_store):
    transport = app.config.get("CONTROLLER_TRANSPORT") or ControllerTransport()
    app.extensions["controller_transport"] = transport
    networks = [ip_network(value.strip()) for value in app.config["ALLOWED_NETWORKS"].split(",") if value.strip()]

    def local_target(address):
        value = IPv4Address(address)
        return any(value in network for network in networks)

    def body():
        if not request.is_json:
            raise model.Problem("Send a JSON object with Content-Type application/json.")
        value = request.get_json()
        if not isinstance(value, dict):
            raise model.Problem("The request body must be a JSON object.")
        return value

    def room(houses, house_id, room_id):
        if house_id not in houses:
            raise model.Problem("Household was not found.", 404, "not_found")
        return model.find(houses[house_id]["rooms"], room_id, "Room")

    def target(houses, house_id, room_id):
        return SETUP_IP if house_id is None else room(houses, house_id, room_id)["controller_ip"]

    def read_status(address):
        if not local_target(address):
            raise ControllerFailure("Controller management is restricted to the configured local network.", 403, "local_only")
        status = clean_status(transport.request(address, "GET", "/status"))
        if status["current_ip"] != address:
            raise ControllerFailure("The controller reported an unexpected current IP.", 409, "controller_identity_mismatch")
        return status

    def probe(address, operation=None):
        addresses = list(dict.fromkeys([operation["new_ip"], operation["old_ip"]])) if operation else [address]
        best, failure = None, None
        for candidate in addresses:
            try:
                status = read_status(candidate)
                if operation and status["device_id"] != operation["device_id"]:
                    failure = ControllerFailure("A different controller answered at the reserved IP.", 409, "controller_identity_mismatch")
                    continue
                if not operation or verified(operation, status):
                    return status
                if best is None:
                    best = status
            except ControllerFailure as error:
                failure = error
        if best is not None:
            return best
        raise failure or ControllerFailure("The controller could not be reached.")

    def save_assignment(houses, operation, desired=True):
        if operation.get("house_id") is None:
            return
        assigned = room(houses, operation["house_id"], operation["room_id"])
        assigned.update(operation["desired_assignment" if desired else "previous_assignment"])
        model.validate_assignments(houses)
        houses_store.save(houses[operation["house_id"]])

    def snapshot(house_id, room_id):
        key = operation_key(house_id, room_id)
        with houses_store.transaction() as houses, operations_store.transaction() as operations:
            return key, target(houses, house_id, room_id), copy.deepcopy(operations.get(key))

    def confirm(key, operation, status):
        if not verified(operation, status):
            return False
        with houses_store.transaction() as houses, operations_store.transaction() as operations:
            current = operations.get(key)
            if not current or current["command_id"] != operation["command_id"]:
                return False
            save_assignment(houses, current)
            del operations[key]
            operations_store.save(operations)
        return True

    def status_response(house_id=None, room_id=None):
        key, address, operation = snapshot(house_id, room_id)
        try:
            status = probe(address, operation)
        except ControllerFailure as error:
            return jsonify(reachable=False, status=None, pending=public_operation(operation), error={"code": error.code, "message": str(error)})
        confirmed_id = operation["command_id"] if operation and confirm(key, operation, status) else None
        return jsonify(reachable=True, status=status, pending=None if confirmed_id else public_operation(operation), confirmed_command_id=confirmed_id)

    def deliver(key, operation):
        payload = {"command_id": operation["command_id"], "device_id": operation["device_id"]}
        if operation["kind"] == "config":
            payload["config"] = operation["config"]
        try:
            acknowledgment = transport.request(operation["delivery_ip"], "POST", "/" + operation["kind"], payload)
            if acknowledgment.get("command_id") != operation["command_id"] or acknowledgment.get("accepted") is not True or acknowledgment.get("restart_required") is not True:
                raise ControllerFailure("The controller did not return a valid command acknowledgment.", 502, "controller_response_invalid")
        except ControllerFailure as error:
            with houses_store.transaction() as houses, operations_store.transaction() as operations:
                current = operations.get(key)
                if current and current["command_id"] == operation["command_id"]:
                    if error.rejected:
                        save_assignment(houses, current, desired=False)
                        del operations[key]
                        operations_store.save(operations)
                        pending = None
                    else:
                        current["state"] = "unconfirmed"
                        operations_store.save(operations)
                        pending = public_operation(current)
                else:
                    pending = None
            return jsonify(error={"code": error.code, "message": str(error)}, pending=pending), error.status
        with houses_store.transaction() as houses, operations_store.transaction() as operations:
            current = operations.get(key)
            if current and current["command_id"] == operation["command_id"]:
                current.update(state="accepted", acknowledged=True)
                operations_store.save(operations)
                pending = public_operation(current)
            else:
                pending = None
        return jsonify(accepted=True, confirmed=pending is None, command_id=operation["command_id"], restart_required=True, pending=pending), 202 if pending else 200

    def mutate(kind, house_id=None, room_id=None):
        raw = body()
        if kind == "config":
            model.patch(raw, CONFIG_FIELDS)
        elif raw:
            raise model.Problem("This command expects an empty JSON object.")
        key, address, pending = snapshot(house_id, room_id)
        if pending:
            raise model.Problem("Verify or retry the pending controller operation first.", 409, "controller_operation_pending")
        status = read_status(address)
        changes = config_patch(raw, status["config"], allow_empty_sensor=house_id is None) if kind == "config" else {}
        if "ip" in changes and not local_target(changes["ip"]):
            raise model.Problem("Controller IP must belong to the configured local network.")
        command_id = str(uuid.uuid4())
        operation = {
            "command_id": command_id, "kind": kind, "state": "prepared", "created_at": app.config["NOW"]().isoformat(),
            "device_id": status["device_id"], "old_ip": address, "new_ip": changes.get("ip", address),
            "delivery_ip": address, "acknowledged": False, "house_id": house_id, "room_id": room_id,
            "config": changes,
        }
        with houses_store.transaction() as houses, operations_store.transaction() as operations:
            if key in operations:
                raise model.Problem("A controller operation is already pending.", 409, "controller_operation_pending")
            if target(houses, house_id, room_id) != address:
                raise model.Problem("The controller assignment changed; refresh and try again.", 409, "controller_assignment_changed")
            if house_id is not None:
                assigned = room(houses, house_id, room_id)
                operation["previous_assignment"] = {name: assigned[name] for name in ("controller_ip", "sensor_mac")}
                operation["desired_assignment"] = dict(operation["previous_assignment"])
                if "ip" in changes:
                    operation["desired_assignment"]["controller_ip"] = changes["ip"]
                if "sensor_mac" in changes:
                    operation["desired_assignment"]["sensor_mac"] = changes["sensor_mac"]
                assigned.update(operation["desired_assignment"])
            operations[key] = operation
            model.validate_assignments(houses)
            validate_reservations(houses, operations)
            # Reserve before changing the household. A delivery never occurs
            # unless both writes finish, and retries recover a prepared write.
            operations_store.save(operations)
            try:
                save_assignment(houses, operation)
            except model.Problem:
                del operations[key]
                operations_store.save(operations)
                raise
        return deliver(key, operation)

    def retry(house_id=None, room_id=None):
        if body():
            raise model.Problem("This command expects an empty JSON object.")
        key, address, operation = snapshot(house_id, room_id)
        if not operation:
            raise model.Problem("There is no unresolved controller operation to retry.", 409, "no_pending_operation")
        status = probe(address, operation)
        if confirm(key, operation, status):
            return jsonify(accepted=True, confirmed=True, command_id=operation["command_id"], restart_required=False, pending=None)
        if status["last_command_id"] == operation["command_id"]:
            return jsonify(accepted=True, confirmed=False, command_id=operation["command_id"], restart_required=True, pending=public_operation(operation)), 202
        with houses_store.transaction() as houses, operations_store.transaction() as operations:
            current = operations.get(key)
            if not current or current["command_id"] != operation["command_id"]:
                raise model.Problem("The pending operation changed; refresh its status.", 409, "controller_operation_changed")
            save_assignment(houses, current)
            current["delivery_ip"] = status["current_ip"]
            # A controller status must never redirect the proxy to another IP.
            if current["delivery_ip"] not in (current["old_ip"], current["new_ip"]):
                raise ControllerFailure("The controller reported an unexpected current IP.", 409, "controller_identity_mismatch")
            operations_store.save(operations)
            operation = copy.deepcopy(current)
        return deliver(key, operation)

    @app.get("/households/<house_id>/rooms/<room_id>/controller/status")
    def room_status(house_id, room_id):
        return status_response(house_id, room_id)

    @app.patch("/households/<house_id>/rooms/<room_id>/controller/config")
    def room_config(house_id, room_id):
        return mutate("config", house_id, room_id)

    @app.post("/households/<house_id>/rooms/<room_id>/controller/restart")
    def room_restart(house_id, room_id):
        return mutate("restart", house_id, room_id)

    @app.post("/households/<house_id>/rooms/<room_id>/controller/retry")
    def room_retry(house_id, room_id):
        return retry(house_id, room_id)

    @app.get("/controllers/setup/status")
    def setup_status():
        return status_response()

    @app.patch("/controllers/setup/config")
    def setup_config():
        return mutate("config")

    @app.post("/controllers/setup/restart")
    def setup_restart():
        return mutate("restart")

    @app.post("/controllers/setup/retry")
    def setup_retry():
        return retry()
