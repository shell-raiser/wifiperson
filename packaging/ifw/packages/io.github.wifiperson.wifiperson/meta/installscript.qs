function Component()
{
}

Component.prototype.createOperations = function()
{
    component.createOperations();

    if (systemInfo.productType === "windows") {
        component.addOperation(
            "CreateShortcut",
            "@TargetDir@/bin/wifiperson.exe",
            "@StartMenuDir@/WiFiperson.lnk");
        component.addOperation(
            "CreateShortcut",
            "@TargetDir@/bin/wifiperson.exe",
            "@DesktopDir@/WiFiperson.lnk");
    }
}
