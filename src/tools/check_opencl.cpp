#include <iostream>
#include <opencv2/core.hpp>
#include <opencv2/core/ocl.hpp>

int main() {
  std::cout << "=== OpenCV Build Information ===\n";
  std::cout << cv::getBuildInformation() << '\n';

  std::cout << "\n=== OpenCL Status ===\n";
  if (cv::ocl::haveOpenCL()) {
    std::cout << "OpenCL Available: YES\n";
    cv::ocl::setUseOpenCL(true);
    const auto& dev = cv::ocl::Device::getDefault();
    std::cout << "Device Name: " << dev.name() << '\n';
    std::cout << "Device Vendor: " << dev.vendorName() << '\n';
    std::cout << "Driver Version: " << dev.driverVersion() << '\n';
    std::cout << "OpenCV Default Device: " << dev.name() << '\n';
  } else {
    std::cout << "OpenCL Available: NO\n";
    std::cout << "Check if libopencv-dev was built with WITH_OPENCL=ON\n";
    std::cout
        << "Check if OpenCL drivers (rocm-opencl / mesa-opencl) are installed.\n";
  }
  return 0;
}
