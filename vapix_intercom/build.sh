APP_IMAGE=vapix_example:1.0
ARCH=aarch64

# `<APP_IMAGE>` is the name to tag the image with, e.g., `vapix_example:1.0`

# `<ARCH>` is the SDK architecture, `armv7hf` or `aarch64`.
rm -rf build
docker build --tag $APP_IMAGE --build-arg ARCH=$ARCH .

# Copy the result from the container image to a local directory `build`:

docker cp $(docker create $APP_IMAGE):/opt/app ./build


