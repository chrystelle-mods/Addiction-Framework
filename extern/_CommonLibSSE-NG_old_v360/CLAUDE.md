# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

CommonLibSSE NG is a modern C++ library for SKSE (Skyrim Script Extender) plugin development that supports multiple Skyrim runtimes: SE (Special Edition), AE (Anniversary Edition), and VR. It provides a comprehensive C++ API for interacting with Skyrim's game engine and is designed to enable single-DLL plugins that work across all supported runtimes.

## Multi-Runtime Architecture

The library uses a unique multi-targeting system that allows plugins to support SE, AE, and VR simultaneously:
- Runtime detection happens at loading time
- Code can probe for runtime-specific features before using them
- Address Library integration handles different game versions automatically
- Conditional compilation flags control runtime support: `ENABLE_SKYRIM_SE`, `ENABLE_SKYRIM_AE`, `ENABLE_SKYRIM_VR`

## Build System Commands

### WSL Environment Requirements
**IMPORTANT**: This project requires Windows-specific tooling (Visual Studio, MSVC compiler) and is designed for Windows development. When working in WSL environments:

- Use PowerShell commands via `powershell.exe` or `pwsh.exe` for build operations
- CMake presets expect Windows paths and Visual Studio toolchain
- Package managers (Vcpkg/Conan) need Windows environment setup

### Configuration and Building
```bash
# WSL: Use PowerShell for CMake operations
powershell.exe "cmake --preset build-debug-msvc-vcpkg-all"
powershell.exe "cmake --preset build-release-msvc-vcpkg-all"
powershell.exe "cmake --preset build-debug-clang-cl-vcpkg-all"

# WSL: Build via PowerShell
powershell.exe "cmake --build build/debug-msvc-vcpkg-all"
powershell.exe "cmake --build build/release-msvc-vcpkg-all"

# WSL: Run tests via PowerShell
powershell.exe "ctest --preset all"
powershell.exe "ctest --preset unit"  # Unit tests only
powershell.exe "ctest --preset integration"  # Integration tests only

# Native Windows (PowerShell/CMD)
cmake --preset build-debug-msvc-vcpkg-all
cmake --build build/debug-msvc-vcpkg-all
ctest --preset all
```

### Package Managers
The project supports both Vcpkg (primary) and Conan:
- **Vcpkg**: Use presets with `-vcpkg-` in the name
- **Conan**: Use presets with `-conan-` in the name

### Runtime-Specific Builds
Available runtime configurations:
- `all`: Supports SE, AE, and VR
- `flatrim`: SE and AE only (no VR)
- `vr`: VR only
- `se`: SE only  
- `ae`: AE only

### Test Categories
- **Unit tests**: `[unit]` - No Skyrim module required
- **Integration tests**: `[integration]` - Skyrim module at rest
- **End-to-end tests**: `[e2e]` - Full Skyrim engine required

## Code Architecture

### Directory Structure
- `include/RE/`: Reverse-engineered Skyrim classes organized alphabetically
- `include/REL/`: Address Library integration and relocation utilities
- `include/REX/`: Cross-platform abstraction layer
- `include/SKSE/`: SKSE plugin framework interfaces
- `src/`: Implementation files mirroring the include structure
- `tests/`: Unit and integration tests using Catch2

### Key Components
- **RE namespace**: Skyrim engine classes (Actor, TESForm, etc.)
- **REL namespace**: Address resolution and version management
- **SKSE namespace**: Plugin lifecycle, event systems, and serialization
- **BST containers**: Bethesda's STL-like container classes
- **Havok integration**: Physics system bindings

### Plugin Declaration
Use the simplified CMake function instead of manual plugin setup:
```cmake
find_package(CommonLibSSE REQUIRED)
add_commonlibsse_plugin(${PROJECT_NAME} SOURCES ${sources})
target_link_libraries(${PROJECT_NAME} PUBLIC CommonLibSSE::CommonLibSSE)
```

## Dependencies and Requirements

### Build Dependencies
- Visual Studio 2022 with C++ Desktop Development
- CMake 3.19+
- Vcpkg or Conan
- Address Library for target runtimes

### C++ Standard
The project uses **C++23** (`cxx_std_23` is set in CMakeLists.txt)

### Address Library Integration
Essential for cross-version compatibility:
- SE: Address Library for SKSE Plugins
- VR: VR Address Library for SKSEVR
- Version detection and offset resolution handled automatically

## Development Workflow

### Making Changes
1. Understand the multi-runtime implications of your changes
2. Use runtime feature detection when accessing version-specific functionality
3. Follow existing patterns for BST containers and Skyrim conventions
4. Run unit tests before integration tests
5. Test across different runtime configurations when possible

### Testing Strategy
- Write unit tests for logic that doesn't require Skyrim
- Use integration tests for Skyrim API interactions
- Reserve e2e tests for full engine functionality
- Tests can load different Skyrim executables for cross-runtime validation

### Code Organization
- Reverse-engineered classes follow Skyrim's naming conventions
- Use RTTI information for class hierarchies and virtual functions
- Address Library IDs are managed through REL::ID system
- Cross-runtime code uses feature detection patterns

### Multi-Runtime Conditional Patterns

CommonLibSSE NG handles runtime differences using two main conditional patterns:

#### Pattern 1: Runtime-Exclusive Virtual Functions

Use this pattern when a class has the **same base class** across runtimes, but **different virtual functions** exist in different runtimes.

**Example:** Camera state classes all inherit from `TESCameraState`, but VR has an extra `Unk_03()` virtual function that SE/AE don't have.

**For runtime-exclusive virtual functions (e.g., VR-only camera state `Unk_03()`):**

**Base Class Pattern:**
```cpp
// Header (.h file) - Three-way conditional pattern
#if defined(EXCLUSIVE_SKYRIM_FLAT)
    // Function doesn't exist in SE/AE-only builds
#elif defined(EXCLUSIVE_SKYRIM_VR)
    virtual void Unk_03();  // 03 - VR only
#else
    void Unk_03();          // 03 - Multi-runtime (non-virtual)
#endif

// Implementation (.cpp file)  
#ifdef SKYRIM_CROSS_VR
    void ClassName::Unk_03()
    {
        if (REL::Module::IsVR()) {
            REL::RelocateVirtual<decltype(&ClassName::Unk_03)>(0x03, 0x03, this);
        }
        // SE/AE: no-op, this function should never be called
    }
#endif
```

**Derived Class Pattern:**
```cpp
// Header (.h file) - Must match base class pattern
#if defined(EXCLUSIVE_SKYRIM_FLAT)
    // Function doesn't exist in SE/AE-only builds
#elif defined(EXCLUSIVE_SKYRIM_VR)
    void Unk_03() override;  // 03 - VR only
#else
    void Unk_03();           // 03 - Multi-runtime
#endif

// Implementation (.cpp file) - Same as base class
#ifdef SKYRIM_CROSS_VR
    void DerivedClass::Unk_03()
    {
        if (REL::Module::IsVR()) {
            REL::RelocateVirtual<decltype(&DerivedClass::Unk_03)>(0x03, 0x03, this);
        }
        // SE/AE: no-op, this function should never be called
    }
#endif
```

**CRITICAL: Vtable Slot Alignment**

When a runtime-exclusive function exists, ALL virtual functions that come after it in the inheritance hierarchy get shifted in the vtable:

```cpp
// SE/AE vtable: Begin(01) -> End(02) -> Update(03) -> GetRotation(04)
// VR vtable:    Begin(01) -> End(02) -> Unk_03(03) -> Update(04) -> GetRotation(05)
```

**Every derived class that overrides functions after a runtime-exclusive function MUST implement RelocateVirtual for those functions:**

```cpp
#ifdef SKYRIM_CROSS_VR
    void DerivedClass::Update(BSTSmartPointer<TESCameraState>& a_nextState)
    {
        REL::RelocateVirtual<decltype(&DerivedClass::Update)>(0x03, 0x04, this, a_nextState);
        //                                                    SE^   VR^ (shifted by Unk_03)
    }

    void DerivedClass::GetRotation(NiQuaternion& a_rotation)
    {
        REL::RelocateVirtual<decltype(&DerivedClass::GetRotation)>(0x04, 0x05, this, a_rotation);
        //                                                         SE^   VR^ (shifted by Unk_03)
    }
#endif
```

**Build behavior:**
- **EXCLUSIVE_SKYRIM_FLAT** (SE/AE-only): Function doesn't exist, no vtable shift
- **EXCLUSIVE_SKYRIM_VR** (VR-only): Function declared as virtual, engine provides implementation
- **Multi-runtime** (ALL/FLATRIM): Function uses RelocateVirtual with runtime detection

**Note:** This pattern works for any runtime-exclusive function (VR-only, SE-only, AE-only, etc.), though VR-only functions are the most common case.

**Do NOT use `SKYRIM_REL_VR_VIRTUAL` for runtime-exclusive functions** - it breaks vtable alignment in multi-runtime builds. Use `SKYRIM_REL_VR_VIRTUAL` only for functions that exist across all runtimes but may need different calling conventions.

#### Pattern 2: Runtime-Exclusive Inheritance

Use this pattern when a class has **completely different base classes** in different runtimes.

**Example:** `ButtonEvent` inherits from `VRWandEvent` in VR but from `IDEvent` in SE/AE. `HUDMenu` inherits from `WorldSpaceMenu` in VR but from `IMenu` in SE/AE.

**Inheritance Pattern:**
```cpp
// Header (.h file) - Three-way conditional inheritance
class ClassName :
#if defined(EXCLUSIVE_SKYRIM_VR)
    public VROnlyBaseClass,
    public SharedBaseClass1,
    public SharedBaseClass2
#elif !defined(ENABLE_SKYRIM_VR)  // SE/AE-only
    public SEAEOnlyBaseClass,
    public SharedBaseClass1,
    public SharedBaseClass2
#else
    // Multi-runtime: can't inherit from incompatible base classes
    public CommonBaseClass  // Choose most compatible base
#endif
{
public:
    // Upcast functions for multi-runtime builds
    [[nodiscard]] VROnlyBaseClass* AsVROnlyBaseClass() noexcept
    {
        if SKYRIM_REL_CONSTEXPR (!REL::Module::IsVR()) {
            return nullptr;
        }
        return &REL::RelocateMember<VROnlyBaseClass>(this, 0, 0);
    }

    [[nodiscard]] SEAEOnlyBaseClass* AsSEAEOnlyBaseClass() noexcept
    {
        if SKYRIM_REL_CONSTEXPR (REL::Module::IsVR()) {
            return nullptr;
        }
        return &REL::RelocateMember<SEAEOnlyBaseClass>(this, 0, 0);
    }
};
```

**Critical:** Use `!defined(ENABLE_SKYRIM_VR)` for SE/AE-only case, not just `#else`, to ensure proper compilation across all preset types.

**Build behavior:**
- **EXCLUSIVE_SKYRIM_VR** (VR-only): Inherits from VR-specific base classes
- **SE/AE-only builds**: Inherits from SE/AE-specific base classes  
- **Multi-runtime** (ALL): Inherits from most compatible base, provides upcast functions that return nullptr when invalid

#### Pattern 3: Chained Inheritance Access

Use this pattern when you need to access base class functionality through a **chain of inheritance** that differs between runtimes.

**Example:** `ButtonEvent` needs access to `IDEvent` members, but:
- **VR**: `ButtonEvent` → `VRWandEvent` → `IDEvent` → `InputEvent`
- **SE/AE**: `ButtonEvent` → `IDEvent` → `InputEvent`
- **Multi-runtime**: `ButtonEvent` → `InputEvent` (no direct inheritance relationship to `IDEvent`)

**Access Function Pattern:**
```cpp
[[nodiscard]] IDEvent* AsIDEvent() noexcept
{
#if defined(EXCLUSIVE_SKYRIM_VR)
    // VR builds: Navigate through VRWandEvent to reach IDEvent
    return static_cast<IDEvent*>(static_cast<VRWandEvent*>(this));
#elif !defined(ENABLE_SKYRIM_VR)
    // SE/AE builds: Direct inheritance from IDEvent
    return static_cast<IDEvent*>(this);
#else
    // Multi-runtime builds: No inheritance relationship, use RelocateMember
    return &REL::RelocateMember<IDEvent>(this, 0, 0);
#endif
}

// Accessor functions that work across all runtimes
[[nodiscard]] std::uint32_t GetIDCode() const noexcept
{
    if (auto idEvent = AsIDEvent()) {
        return idEvent->idCode;
    }
    return 0;  // Fallback for invalid cases
}

void SetIDCode(std::uint32_t a_idCode)
{
    if (auto idEvent = AsIDEvent()) {
        idEvent->idCode = a_idCode;
    }
}
```

**Key Points:**
- **Single-runtime builds**: Use efficient `static_cast` following known inheritance chains
- **Multi-runtime builds**: Use `RelocateMember` since inheritance relationship doesn't exist at compile time
- **Always validate**: Check return values from upcast functions for safety
- **Consistent API**: Accessor functions provide the same interface regardless of runtime

**Build behavior:**
- **EXCLUSIVE_SKYRIM_VR**: Uses double static_cast through inheritance chain
- **SE/AE-only builds**: Uses direct static_cast  
- **Multi-runtime**: Uses RelocateMember for runtime-specific memory layout access

#### Multi-Runtime Architecture Patterns

CommonLibSSE NG uses different abstraction patterns based on the type and complexity of runtime differences:

**Virtual Functions (Explicit Pattern - Recommended):**
- Use three-way conditionals for clarity and maintainability
- Virtual functions are relatively rare, so explicit patterns don't create significant maintenance burden
- Clear separation between runtime-exclusive and cross-runtime functions

**Data Structures (Macro Pattern - Established):**
- Use `RUNTIME_DATA_CONTENT` macros for complex structural differences
- Beneficial when multiple large structs have significant layout differences
- Examples: `HUDMenu`, `State`, `BGSShaderParticleGeometryData`

**Inheritance (Three-way Conditional):**
- Use explicit conditionals for different base class hierarchies
- Example: VR's `WorldSpaceMenu` vs SE/AE's `IMenu` inheritance

**Individual Members (Mixed Approach):**
- Simple exclusions: `#ifndef SKYRIM_CROSS_VR`
- Type differences: Three-way conditionals
- Runtime detection: Use `REL::Module::IsVR()` when both layouts exist

**Maintainability Guidelines:**
- Prefer explicit patterns for infrequent changes (virtual functions)
- Use macro abstraction for repetitive complex patterns (data structures)
- Document offset calculations and Address Library relationships
- Standardize member accessor patterns using `RelocateMember`