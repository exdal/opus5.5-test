-- oxcity override: fetch-only recipe. the upstream recipe builds libXrandr from x.org tarballs, which the
-- sandbox proxy blocks. tools/sandbox/bootstrap.sh extracts Ubuntu's libxrandr-dev into .sandbox/sysroot
-- instead, and this recipe hands xmake those headers and libs
package("libxrandr")
    set_homepage("https://www.x.org/")
    set_description("X11 libXrandr (from the oxcity sandbox sysroot)")

    on_fetch("linux", function (package, opt)
        local sandbox = os.getenv("OXCITY_SANDBOX")
        if not sandbox then
            return
        end
        local sysroot = path.join(sandbox, "sysroot")
        local libdir = path.join(sysroot, "usr", "lib", "x86_64-linux-gnu")
        if os.isfile(path.join(libdir, "libXrandr.so")) then
            return {includedirs = {path.join(sysroot, "usr", "include")}, linkdirs = {libdir}, links = {"Xrandr"}}
        end
    end)
