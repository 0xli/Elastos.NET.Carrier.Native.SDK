"""Run one Elastos.CarrierClassic.Express node for the multi-node test.

Starts the real Express Flask app (express.py from the Express repo) on
127.0.0.1:<port> over TLS with its own private key, its own sqlite file and
its own storage directory. Run it with the Express repo's venv python:

    venv/bin/python express_node.py --express-root ~/StudioProjects/Elastos.CarrierClassic.Express \
        --port 15005 --sk <base58 secret key> --db /tmp/a.db --storage /tmp/a-files \
        --cert cert.pem --key key.pem

The SDK speaks https:// only and does not verify the certificate
(http_client.c sets CURLOPT_SSL_VERIFYPEER 0), so a self-signed cert works.
"""
import argparse
import os
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--express-root', required=True)
    ap.add_argument('--port', type=int, required=True)
    ap.add_argument('--sk', required=True, help='node private key, base58')
    ap.add_argument('--db', required=True, help='sqlite file')
    ap.add_argument('--storage', required=True, help='file storage dir')
    ap.add_argument('--cert', required=True)
    ap.add_argument('--key', required=True)
    args = ap.parse_args()

    root = os.path.abspath(args.express_root)
    os.chdir(root)
    sys.path.insert(0, root)
    os.environ['EXPRESS_PRIVATE_KEY'] = args.sk
    os.environ.setdefault('EXPRESS_PUSH_MODE', 'off')

    import config
    config.Config.SQLALCHEMY_DATABASE_URI = 'sqlite:///' + os.path.abspath(args.db)
    config.Config.STORAGE_PATH = os.path.abspath(args.storage)
    os.makedirs(config.Config.STORAGE_PATH, exist_ok=True)

    import express  # noqa: E402  (builds the Flask app at import)
    from app import db

    with express.application.app_context():
        db.create_all()

    print('express node ready port={} pk={}'.format(
        args.port, express.application.publicKey), flush=True)
    express.application.run(host='127.0.0.1', port=args.port,
                            ssl_context=(args.cert, args.key),
                            debug=False, threaded=True, use_reloader=False)


if __name__ == '__main__':
    main()
