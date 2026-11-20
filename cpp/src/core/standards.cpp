#include "standards.h"

#include "errors.h"

namespace lr::standards {

const std::vector<Standard>& all() {
    // name, kind, source, note, aka, bands
    static const std::vector<Standard> s = {
        {"cyclohexane", "liquid", "ASTM", "", {},
         {{384.1, M}, {426.3, M}, {801.3, S}, {1028.3, S}, {1157.6, M}, {1266.4, M}, {1444.4, S}, {2664.4, W},
          {2852.9, S}, {2923.8, S}, {2938.3, S}}},
        {"naphthalene", "solid", "ASTM", "", {},
         {{513.8, M}, {763.8, S}, {1021.6, M}, {1147.2, M}, {1382.2, S}, {1464.5, M}, {1576.6, M}, {3056.4, M}}},
        {"sulfur", "solid", "ASTM", "", {},
         {{153.8, S}, {219.1, S}, {473.2, S}}},
        {"polystyrene", "solid", "ASTM", "", {},
         {{620.9, M}, {795.8, M}, {1001.4, S}, {1031.8, M}, {1155.3, M}, {1450.5, M}, {1583.1, M}, {1602.3, S},
          {2852.4, W}, {2904.5, M}, {3054.3, S}}},
        {"paracetamol", "solid", "ASTM", "a plain tablet: cheap multi-point calibration", {"4-acetamidophenol", "acetaminophen"},
         {{213.3, M}, {329.2, M}, {390.9, M}, {465.1, M}, {504.0, M}, {651.6, M}, {710.8, M}, {797.2, M},
          {834.5, M}, {857.9, S}, {968.7, W}, {1105.5, M}, {1168.5, S}, {1236.8, S}, {1278.5, M}, {1323.9, S},
          {1371.5, M}, {1515.1, W}, {1561.5, M}, {1648.4, S}, {2931.1, M}, {3064.6, M}}},
        {"benzonitrile", "liquid", "ASTM", "", {},
         {{460.9, M}, {548.5, M}, {751.3, M}, {767.1, M}, {1000.7, S}, {1026.6, M}, {1177.9, M}, {1192.6, M},
          {1598.9, S}, {2229.4, S}, {3072.3, M}}},
        {"toluene/acetonitrile 50:50", "liquid", "ASTM", "", {"toluene-acetonitrile"},
         {{378.5, M}, {521.7, M}, {786.5, S}, {919.0, M}, {1003.6, S}, {1030.6, M}, {1211.4, M}, {1376.4, M},
          {1605.1, M}, {2253.7, S}, {2940.8, S}, {3057.1, M}}},
        {"indene", "liquid", "ASTM", "", {},
         {{730.4, S}, {1018.6, M}, {1205.6, M}, {1552.6, S}, {1610.2, M}, {3054.7, M}}},
        {"carbon tetrachloride", "liquid", "lit", "", {"ccl4", "tetrachloromethane"},
         {{218.0, S}, {314.0, S}, {459.0, S}, {762.0, M}, {790.0, M}}},
        {"chloroform", "liquid", "lit", "", {"chcl3", "trichloromethane"},
         {{261.5, M}, {364.0, S}, {667.0, S}, {760.0, M}, {1216.0, M}, {3019.0, S}}},
        {"dichloromethane", "liquid", "lit", "", {"dcm", "ch2cl2", "methylene chloride"},
         {{282.0, S}, {704.0, S}, {737.0, M}, {1157.0, W}, {1423.0, W}, {2987.0, S}, {3047.0, W}}},
        {"toluene", "liquid", "lit", "", {},
         {{521.7, M}, {786.5, S}, {1003.6, S}, {1030.6, M}, {1211.4, M}, {1605.1, M}, {2920.0, M}, {3052.8, M}}},
        {"benzene", "liquid", "lit", "", {},
         {{606.0, M}, {992.0, S}, {1178.0, M}, {1585.0, M}, {3062.0, S}}},
        {"acetonitrile", "liquid", "lit", "", {"mecn"},
         {{379.0, M}, {918.0, S}, {1376.0, M}, {2249.0, S}, {2942.0, S}}},
        {"acetone", "liquid", "lit", "", {},
         {{787.0, S}, {1066.0, M}, {1430.0, M}, {1710.0, M}, {2925.0, S}}},
        {"ethanol", "liquid", "lit", "", {"etoh"},
         {{433.0, M}, {882.0, S}, {1052.0, M}, {1096.0, M}, {1454.0, M}, {2878.0, S}, {2930.0, S}, {2974.0, S}}},
        {"methanol", "liquid", "lit", "", {"meoh"},
         {{1033.0, S}, {1453.0, M}, {2837.0, S}, {2945.0, S}}},
        {"isopropanol", "liquid", "lit", "", {"2-propanol", "ipa"},
         {{819.0, S}, {954.0, M}, {1129.0, M}, {1453.0, M}, {2884.0, S}, {2938.0, S}, {2975.0, S}}},
        {"dmso", "liquid", "lit", "", {"dimethyl sulfoxide"},
         {{667.0, S}, {698.0, S}, {953.0, M}, {1042.0, M}, {1419.0, M}, {2913.0, S}, {2997.0, M}}},
        {"ethylene glycol", "liquid", "lit", "", {},
         {{482.0, M}, {523.0, M}, {864.0, S}, {1036.0, M}, {1089.0, M}, {1463.0, M}, {2880.0, S}, {2942.0, S}}},
        {"cyclohexane (lit)", "liquid", "lit", "", {},
         {{801.0, S}, {1028.0, S}, {1444.0, S}, {2853.0, S}, {2938.0, S}}},
        {"silicon", "solid", "lit", "single sharp band: one-point (offset) check; 520.5-520.7 depending on temperature", {"si", "si wafer"},
         {{520.5, S}}},
        {"diamond", "solid", "lit", "", {},
         {{1332.5, S}}},
        {"calcite", "solid", "lit", "", {"caco3"},
         {{155.0, M}, {281.0, M}, {711.0, M}, {1085.5, S}}},
        {"anatase", "solid", "lit", "", {"tio2 anatase"},
         {{144.0, S}, {197.0, W}, {399.0, M}, {513.0, M}, {639.0, M}}},
        {"rutile", "solid", "lit", "", {"tio2 rutile"},
         {{447.0, S}, {612.0, S}}},
        {"ptfe", "solid", "lit", "", {"teflon"},
         {{292.0, M}, {386.0, M}, {576.0, W}, {732.0, S}, {1216.0, M}, {1299.0, M}, {1381.0, M}}},
        {"barium sulfate", "solid", "lit", "", {"baso4"},
         {{453.0, M}, {461.0, M}, {988.0, S}}},
    };
    return s;
}

const Standard& find(const QString& name) {
    const QString key = name.trimmed().toLower();
    for (const Standard& s : all()) {
        if (key == s.name) return s;
        for (const QString& a : s.aka)
            if (key == a.toLower()) return s;
    }
    QStringList known;
    for (const Standard& s : all()) known << s.name;
    known.sort();
    // Python repr() of the name: single quotes unless it contains one (and no double quote)
    const QString q = name.contains('\'') && !name.contains('"') ? "\"" : "'";
    throw Error(("unknown standard " + q + name + q + "; known: " + known.join(", ")).toStdString());
}

std::vector<Band> bands(const QString& name) {
    const Standard& s = find(name);
    const double tol = s.source == "ASTM" ? 0.5 : 2.0;
    std::vector<Band> out = s.bands;
    for (Band& b : out)
        if (b.tolerance < 0) b.tolerance = tol;
    return out;
}

}  // namespace lr::standards
