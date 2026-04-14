## AetherCore: Modern Vulkan 1.4


### 🛠 Tech Stack

* **Graphics API:** Vulkan 1.4 
* **Windowing:** GLFW 3.4
* **Math:** GLM 1.0.3
* **Utilities:** `vk-bootstrap` for simplified device initialization

---

### 📂 Project Structure

* `src/engine/`: Core engine logic
* `src/main.cpp`: Application entry point and frame loop.
* `CMake/`: Custom build scripts and dependency management.

---

### 🔨 Building

1.  Ensure you have the **Vulkan SDK (1.3.275+)** installed.
2.  Clone the repository.
3.  Run the following commands:

```bash
mkdir build && cd build
cmake ..
cmake --build .
```
