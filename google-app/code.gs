const CONFIG = {
  SHEET_NAME: "Sheet1",
  SECRET_TOKEN: "QNX_Pi5_2026_a8F3kL9vR2xT7mN5pW4z"
};

function doPost(e) {

  const lock = LockService.getScriptLock();
  let locked = false;

  try {

    if (!e || !e.postData || !e.postData.contents) {
      throw new Error("Missing request data");
    }

    const data = JSON.parse(e.postData.contents);

    if (data.token !== CONFIG.SECRET_TOKEN) {
      return respond({
        success: false,
        error: "Unauthorized"
      });
    }

    if (!Array.isArray(data.rows)) {
      throw new Error("Rows missing");
    }

    if (data.rows.length === 0) {
      throw new Error("No rows");
    }

    if (data.rows.length > 500) {
      throw new Error("Too many rows");
    }

    const out = data.rows.map(function(row) {

      if (!Array.isArray(row) || row.length !== 6) {
        throw new Error("Invalid row: " + JSON.stringify(row));
      }

      const ts = row[0];
      const hr = Number(row[1]);
      const temp = Number(row[2]);
      const hum = Number(row[3]);
      const ntc = Number(row[4]);
      const status = row[5];

      const m = ts.match(
        /^(\d{4})-(\d{2})-(\d{2})[ T](\d{2}):(\d{2}):(\d{2})$/
      );

      if (!m) {
        throw new Error("Invalid timestamp: " + ts);
      }

      if (
        !Number.isFinite(hr) ||
        !Number.isFinite(temp) ||
        !Number.isFinite(hum) ||
        !Number.isFinite(ntc)
      ) {
        throw new Error("Invalid sensor values");
      }

      if (status !== "live" && status !== "local") {
        throw new Error("Invalid status");
      }

      return [

        // Date
        new Date(
          Number(m[1]),
          Number(m[2]) - 1,
          Number(m[3])
        ),

        // Time
        new Date(
          1899,
          11,
          30,
          Number(m[4]),
          Number(m[5]),
          Number(m[6])
        ),

        // Internet
        status,

        // Heart Rate
        hr,

        // Temperature
        temp,

        // Humidity
        hum,

        // NTC
        ntc
      ];
    });

    lock.waitLock(10000);
    locked = true;

    const sheet = SpreadsheetApp
      .getActiveSpreadsheet()
      .getSheetByName(CONFIG.SHEET_NAME);

    if (!sheet) {
      throw new Error("Sheet1 not found");
    }

    const startRow =
      Math.max(sheet.getLastRow() + 1, 2);

    sheet.getRange(
      startRow,
      1,
      out.length,
      7
    ).setValues(out);

    sheet.getRange(
      startRow,
      1,
      out.length,
      1
    ).setNumberFormat("yyyy-mm-dd");

    sheet.getRange(
      startRow,
      2,
      out.length,
      1
    ).setNumberFormat("hh:mm:ss");

    return respond({
      success: true,
      rows: out.length
    });

  } catch (err) {

    return respond({
      success: false,
      error: String(err.message || err)
    });

  } finally {

    if (locked && lock.hasLock()) {
      lock.releaseLock();
    }
  }
}

function respond(obj) {

  return ContentService
    .createTextOutput(
      JSON.stringify(obj)
    )
    .setMimeType(
      ContentService.MimeType.JSON
    );
}

function authorizeSheet() {

  const sheet =
    SpreadsheetApp
      .getActiveSpreadsheet()
      .getSheetByName(
        CONFIG.SHEET_NAME
      );

  if (!sheet) {
    throw new Error(
      "Sheet1 not found"
    );
  }

  Logger.log(
    "Authorization successful"
  );
}