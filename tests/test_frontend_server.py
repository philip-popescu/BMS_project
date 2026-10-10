from backend.frontend_server import create_frontend


def test_static_application_assets_and_local_access(tmp_path):
    (tmp_path / "assets").mkdir()
    (tmp_path / "index.html").write_text("<html>BMS</html>")
    (tmp_path / "assets" / "app.js").write_text("console.log('BMS')")
    app = create_frontend({"TESTING": True, "DIST_DIR": str(tmp_path)})
    client = app.test_client()
    assert client.get("/").data == b"<html>BMS</html>"
    assert client.get("/households/home").status_code == 200
    assert "immutable" in client.get("/assets/app.js").headers["Cache-Control"]
    assert client.get("/assets/missing.js").status_code == 404
    assert client.get("/.env").status_code == 404
    assert client.get("/../.env").status_code == 404
    assert client.get("/", environ_overrides={"REMOTE_ADDR": "8.8.8.8"}).status_code == 403
