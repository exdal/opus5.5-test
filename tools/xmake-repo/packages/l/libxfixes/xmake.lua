-- oxcity override: fetch-only recipe. the upstream recipe builds libXfixes from x.org tarballs, which the
-- sandbox proxy blocks. tools/sandbox/bootstrap.sh extracts Ubuntu's libxfixes-dev into .sandbox/sysroot
-- instead, and this recipe hands xmake those headers and libs
package("libxfixes")
    set_homepage("https://www.x.org/")
    set_description("X11 libXfixes (from the oxcity sandbox sysroot)")

    on_fetch("linux", function (package, opt)
        local sandbox = os.getenv("OXCITY_SANDBOX")
        if not sandbox then
            return
        end
        local sysroot = path.join(sandbox, "sysroot")
        local libdir = path.join(sysroot, "usr", "lib", "x86_64-linux-gnu")
        if os.isfile(path.join(libdir, "libXfixes.so")) then
            return {includedirs = {path.join(sysroot, "usr", "include")}, linkdirs = {libdir}, links = {"Xfixes"}}
        end
    end)
