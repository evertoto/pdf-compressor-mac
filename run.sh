set -e

cd "$(dirname "$0")"

echo "Building PDF Compressor.app..."

if ! make app; then
    echo "Error: Build failed."
    exit 1
fi

open "$(dirname "$0")/PDF Compressor.app"

if [ $? -eq 0 ]; then
    echo "PDF Compressor.app opened successfully."
else
    echo "Error: Failed to open PDF Compressor.app"
    exit 1
fi
