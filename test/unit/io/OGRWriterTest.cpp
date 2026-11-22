/******************************************************************************
* Copyright (c) 2016, Hobu Inc. (info@hobu.co)
*
* All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following
* conditions are met:
*
*     * Redistributions of source code must retain the above copyright
*       notice, this list of conditions and the following disclaimer.
*     * Redistributions in binary form must reproduce the above copyright
*       notice, this list of conditions and the following disclaimer in
*       the documentation and/or other materials provided
*       with the distribution.
*     * Neither the name of Hobu, Inc. nor the
*       names of its contributors may be used to endorse or promote
*       products derived from this software without specific prior
*       written permission.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
* "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
* LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
* FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
* COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
* INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
* BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
* OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
* AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
* OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
* OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
* OF SUCH DAMAGE.
****************************************************************************/

#include <pdal/pdal_test_main.hpp>
#include <pdal/util/FileUtils.hpp>
#include <pdal/private/gdal/ErrorHandler.hpp>
#include <filters/HeadFilter.hpp>
#include <filters/SortFilter.hpp>
#include <io/OGRWriter.hpp>
#include <io/LasReader.hpp>
#include "Support.hpp"

#include <gdal_priv.h>
#include <ogrsf_frmts.h>

#include <string>
#include <vector>

// These tests write points with writers.ogr, then open the result with GDAL
// and check the layer and its features: geometry type, feature count,
// extent, SRS, fields, and the first feature's values. They used to compare
// ogrinfo's text output with stored copies instead, which broke whenever
// GDAL or PROJ changed how ogrinfo prints things.

namespace pdal
{

namespace
{

// Writes the first featureCount points of infile, sorted by X, to
// temp/ogr/<name> with writers.ogr and the given options, and returns the
// path written.
std::string writeOgr(const Options& wo, const std::string& infile,
    const std::string& name, int featureCount = 10)
{
    FileUtils::createDirectory(Support::temppath("ogr"));
    std::string outfile(Support::temppath("ogr/" + name));
    if (FileUtils::isDirectory(outfile))
        FileUtils::deleteDirectory(outfile);
    else
        FileUtils::deleteFile(outfile);

    Options readerOpts;
    readerOpts.add("filename", infile);
    LasReader reader;
    reader.setOptions(readerOpts);

    Options sortOpts;
    sortOpts.add("dimension", "X");
    SortFilter sortFilter;
    sortFilter.setOptions(sortOpts);
    sortFilter.setInput(reader);

    Options headOpts;
    headOpts.add("count", featureCount);
    HeadFilter headFilter;
    headFilter.setOptions(headOpts);
    headFilter.setInput(sortFilter);

    Options writerOpts;
    writerOpts.add("filename", outfile);
    OGRWriter writer;
    writer.setOptions(wo);
    writer.addOptions(writerOpts);
    writer.setInput(headFilter);

    PointTable t;
    writer.prepare(t);
    writer.execute(t);

    return outfile;
}

struct Field
{
    std::string name;
    OGRFieldType type;
    double value;  // in the first feature
};

struct Expected
{
    OGRwkbGeometryType geomType;
    GIntBig featureCount;
    OGREnvelope extent;
    // X, Y, Z of the first point of the first feature, plus M when the
    // geometry should be measured
    std::vector<double> firstPoint;
    // the fields writers.ogr should create, in order
    std::vector<Field> fields;
    // points in each feature, for multipoint layers
    std::vector<int> pointsPerFeature;
    // nullptr: don't check the SRS; "": there should be none
    const char *srsName;
};

OGREnvelope envelope(double minx, double miny, double maxx, double maxy)
{
    OGREnvelope env;
    env.MinX = minx;
    env.MinY = miny;
    env.MaxX = maxx;
    env.MaxY = maxy;
    return env;
}

// The first 10 points of las/simple.las, sorted by X
const OGREnvelope simpleExtent(
    envelope(635619.85, 849017.32, 635685.33, 852165.12));
const std::vector<double> simpleFirstPoint { 635619.85, 850064.04, 447.01 };

// The non-XYZ dimensions of las/simple.las, with the first point's values.
// Shapefile cuts field names to 10 characters.
std::vector<Field> simpleFields(bool shapefile, bool withClassification)
{
    struct Dim
    {
        const char *name;
        const char *shpName;
        OGRFieldType type;
        double value;
    };
    static const Dim dims[] = {
        { "Intensity", "Intensity", OFTInteger, 1 },
        { "ReturnNumber", "ReturnNumb", OFTInteger, 1 },
        { "NumberOfReturns", "NumberOfRe", OFTInteger, 2 },
        { "ScanDirectionFlag", "ScanDirect", OFTInteger, 0 },
        { "EdgeOfFlightLine", "EdgeOfFlig", OFTInteger, 0 },
        { "Classification", "Classifica", OFTInteger, 1 },
        { "Synthetic", "Synthetic", OFTInteger, 0 },
        { "KeyPoint", "KeyPoint", OFTInteger, 0 },
        { "Withheld", "Withheld", OFTInteger, 0 },
        { "Overlap", "Overlap", OFTInteger, 0 },
        { "ScanAngleRank", "ScanAngleR", OFTReal, 15 },
        { "UserData", "UserData", OFTInteger, 125 },
        { "PointSourceId", "PointSourc", OFTInteger, 7327 },
        { "GpsTime", "GpsTime", OFTReal, 246092.20788099366473 },
        { "Red", "Red", OFTInteger, 52 },
        { "Green", "Green", OFTInteger, 65 },
        { "Blue", "Blue", OFTInteger, 68 }
    };

    std::vector<Field> fields;
    for (const Dim& d : dims)
    {
        if (!withClassification && std::string(d.name) == "Classification")
            continue;
        fields.push_back({ shapefile ? d.shpName : d.name, d.type, d.value });
    }
    return fields;
}

void checkLayer(const std::string& filename, const Expected& exp)
{
    GDALDatasetUniquePtr ds(GDALDataset::Open(filename.c_str(),
        GDAL_OF_VECTOR | GDAL_OF_READONLY));
    ASSERT_TRUE(ds) << "can't open " << filename;
    ASSERT_EQ(ds->GetLayerCount(), 1);
    OGRLayer *layer = ds->GetLayer(0);

    EXPECT_EQ(layer->GetGeomType(), exp.geomType)
        << "got " << OGRGeometryTypeToName(layer->GetGeomType())
        << ", expected " << OGRGeometryTypeToName(exp.geomType);
    EXPECT_EQ(layer->GetFeatureCount(TRUE), exp.featureCount);

    OGREnvelope env;
    ASSERT_EQ(layer->GetExtent(&env, TRUE), OGRERR_NONE);
    EXPECT_NEAR(env.MinX, exp.extent.MinX, 1e-6);
    EXPECT_NEAR(env.MinY, exp.extent.MinY, 1e-6);
    EXPECT_NEAR(env.MaxX, exp.extent.MaxX, 1e-6);
    EXPECT_NEAR(env.MaxY, exp.extent.MaxY, 1e-6);

    if (exp.srsName)
    {
        const OGRSpatialReference *srs = layer->GetSpatialRef();
        if (*exp.srsName == '\0')
            EXPECT_EQ(srs, nullptr) << "unexpected SRS " << srs->GetName();
        else
        {
            ASSERT_NE(srs, nullptr);
            EXPECT_STREQ(srs->GetName(), exp.srsName);
        }
    }

    // A Shapefile needs at least one field, so the driver adds "FID" to a
    // layer without any. That's not from writers.ogr, so skip it.
    OGRFeatureDefn *defn = layer->GetLayerDefn();
    std::vector<std::pair<std::string, OGRFieldType>> fields;
    for (int i = 0; i < defn->GetFieldCount(); ++i)
    {
        OGRFieldDefn *fd = defn->GetFieldDefn(i);
        if (std::string(fd->GetNameRef()) != "FID")
            fields.emplace_back(fd->GetNameRef(), fd->GetType());
    }
    ASSERT_EQ(fields.size(), exp.fields.size());
    for (size_t i = 0; i < fields.size(); ++i)
    {
        EXPECT_EQ(fields[i].first, exp.fields[i].name);
        EXPECT_EQ(fields[i].second, exp.fields[i].type)
            << "field " << fields[i].first;
    }

    layer->ResetReading();
    OGRFeatureUniquePtr feat(layer->GetNextFeature());
    ASSERT_TRUE(feat);
    for (const Field& f : exp.fields)
        EXPECT_NEAR(feat->GetFieldAsDouble(f.name.c_str()), f.value, 1e-6)
            << "field " << f.name;

    const OGRGeometry *geom = feat->GetGeometryRef();
    ASSERT_NE(geom, nullptr);
    const OGRPoint *pt;
    if (wkbFlatten(geom->getGeometryType()) == wkbMultiPoint)
    {
        const OGRMultiPoint *mp = geom->toMultiPoint();
        ASSERT_GT(mp->getNumGeometries(), 0);
        pt = mp->getGeometryRef(0);
    }
    else
    {
        ASSERT_EQ(wkbFlatten(geom->getGeometryType()), wkbPoint);
        pt = geom->toPoint();
    }
    ASSERT_TRUE(exp.firstPoint.size() == 3 || exp.firstPoint.size() == 4);
    EXPECT_NEAR(pt->getX(), exp.firstPoint[0], 1e-6);
    EXPECT_NEAR(pt->getY(), exp.firstPoint[1], 1e-6);
    EXPECT_TRUE(pt->Is3D());
    EXPECT_NEAR(pt->getZ(), exp.firstPoint[2], 1e-6);
    EXPECT_EQ(pt->IsMeasured(), exp.firstPoint.size() == 4);
    if (exp.firstPoint.size() == 4)
        EXPECT_NEAR(pt->getM(), exp.firstPoint[3], 1e-6);

    if (exp.pointsPerFeature.size())
    {
        std::vector<int> counts;
        layer->ResetReading();
        for (auto& f : *layer)
        {
            const OGRGeometry *g = f->GetGeometryRef();
            ASSERT_NE(g, nullptr);
            ASSERT_EQ(wkbFlatten(g->getGeometryType()), wkbMultiPoint);
            counts.push_back(g->toMultiPoint()->getNumGeometries());
        }
        EXPECT_EQ(counts, exp.pointsPerFeature);
    }
}

} // unnamed namespace

TEST(OGRWriterTest, shapefile)
{
    Options wo;
    wo.add("ogrdriver", "ESRI Shapefile");
    std::string out = writeOgr(wo, Support::datapath("las/simple.las"),
        "shapefile.shp");

    // writers.ogr creates the layer as Point ZM, but a Shapefile reports M
    // only if the features have M values
    checkLayer(out, { wkbPoint25D, 10, simpleExtent, simpleFirstPoint,
        {}, {}, "" });
}

TEST(OGRWriterTest, json)
{
    Options wo;
    wo.add("ogrdriver", "GeoJSON");
    std::string out = writeOgr(wo, Support::datapath("las/simple.las"),
        "json.geojson");

    // GeoJSON has no M, and reports WGS 84 for any file without a CRS
    checkLayer(out, { wkbPoint25D, 10, simpleExtent, simpleFirstPoint,
        {}, {}, nullptr });
}

TEST(OGRWriterTest, geopackage)
{
    Options wo;
    wo.add("ogrdriver", "GPKG");
    std::string out = writeOgr(wo, Support::datapath("las/utm15.las"),
        "geopackage.gpkg");

    // GeoPackage keeps the layer's Point ZM type
    checkLayer(out, { wkbPointZM, 1,
        envelope(470692.44, 4602888.9, 470692.44, 4602888.9),
        { 470692.44, 4602888.9, 16 }, {}, {}, "NAD83 / UTM zone 15N" });
}

TEST(OGRWriterTest, creation_options)
{
    Options wo;
    wo.add("ogrdriver", "GeoJSON");
    wo.add("ogr_options", "WRITE_BBOX=YES");
    wo.add("ogr_options", "COORDINATE_PRECISION=1");
    std::string out = writeOgr(wo, Support::datapath("las/simple.las"),
        "creation_options.geojson");

    // COORDINATE_PRECISION=1 rounds coordinates to one decimal place
    checkLayer(out, { wkbPoint25D, 10,
        envelope(635619.8, 849017.3, 635685.3, 852165.1),
        { 635619.8, 850064, 447 }, {}, {}, nullptr });

    // WRITE_BBOX=YES adds a bbox member to each feature
    std::string text = FileUtils::readFileIntoString(out);
    EXPECT_NE(text.find("\"bbox\""), std::string::npos);
}

TEST(OGRWriterTest, shapefile_measure)
{
    Options wo;
    wo.add("ogrdriver", "ESRI Shapefile");
    wo.add("measure_dim", "Classification");
    std::string out = writeOgr(wo, Support::datapath("las/simple.las"),
        "shapefile_measure.shp");

    checkLayer(out, { wkbPointZM, 10, simpleExtent,
        { 635619.85, 850064.04, 447.01, 1 }, {}, {}, "" });
}

TEST(OGRWriterTest, attrs_all)
{
    Options wo;
    wo.add("ogrdriver", "ESRI Shapefile");
    wo.add("attr_dims", "all");
    std::string out = writeOgr(wo, Support::datapath("las/simple.las"),
        "attrs_all.shp");

    checkLayer(out, { wkbPoint25D, 10, simpleExtent, simpleFirstPoint,
        simpleFields(true, true), {}, "" });
}

TEST(OGRWriterTest, geopackage_attrs_all)
{
    Options wo;
    wo.add("ogrdriver", "GPKG");
    wo.add("attr_dims", "all");
    std::string out = writeOgr(wo, Support::datapath("las/simple.las"),
        "geopackage_attrs_all.gpkg");

    // A GeoPackage without a CRS reads back with an "Undefined geographic
    // SRS" before GDAL 3.9 and none after, so the SRS isn't checked
    checkLayer(out, { wkbPointZM, 10, simpleExtent, simpleFirstPoint,
        simpleFields(false, true), {}, nullptr });
}

TEST(OGRWriterTest, attrs)
{
    Options wo;
    wo.add("ogrdriver", "ESRI Shapefile");
    wo.add("attr_dims", "Classification,Red,Green,Blue");
    std::string out = writeOgr(wo, Support::datapath("las/simple.las"),
        "attrs.shp");

    checkLayer(out, { wkbPoint25D, 10, simpleExtent, simpleFirstPoint,
        { { "Classifica", OFTInteger, 1 }, { "Red", OFTInteger, 52 },
          { "Green", OFTInteger, 65 }, { "Blue", OFTInteger, 68 } },
        {}, "" });
}

TEST(OGRWriterTest, attrs_measure)
{
    Options wo;
    wo.add("ogrdriver", "ESRI Shapefile");
    wo.add("measure_dim", "Classification");
    wo.add("attr_dims", "all");
    std::string out = writeOgr(wo, Support::datapath("las/simple.las"),
        "attrs_measure.shp");

    // "all" leaves out the measure dimension, as it's already the M value
    checkLayer(out, { wkbPointZM, 10, simpleExtent,
        { 635619.85, 850064.04, 447.01, 1 }, simpleFields(true, false),
        {}, "" });
}

TEST(OGRWriterTest, multicount)
{
    Options wo;
    wo.add("ogrdriver", "ESRI Shapefile");
    wo.add("multicount", "3");
    std::string out = writeOgr(wo, Support::datapath("las/simple.las"),
        "multicount.shp");

    // 10 points in threes, with the remaining point in a last feature
    checkLayer(out, { wkbMultiPoint25D, 4, simpleExtent, simpleFirstPoint,
        {}, { 3, 3, 3, 1 }, "" });
}

TEST(OGRWriterTest, multicount_2)
{
    // feature count < multicount
    Options wo;
    wo.add("ogrdriver", "ESRI Shapefile");
    wo.add("multicount", "20");
    std::string out = writeOgr(wo, Support::datapath("las/simple.las"),
        "multicount_2.shp");

    checkLayer(out, { wkbMultiPoint25D, 1, simpleExtent, simpleFirstPoint,
        {}, { 10 }, "" });
}

TEST(OGRWriterTest, multicount_3)
{
    // feature count == exact multiple of multicount
    Options wo;
    wo.add("ogrdriver", "ESRI Shapefile");
    wo.add("multicount", "5");
    std::string out = writeOgr(wo, Support::datapath("las/simple.las"),
        "multicount_3.shp");

    checkLayer(out, { wkbMultiPoint25D, 2, simpleExtent, simpleFirstPoint,
        {}, { 5, 5 }, "" });
}

TEST(OGRWriterTest, error_multicount_attrs)
{
    Options wo;
    wo.add("ogrdriver", "GeoJSON");
    wo.add("multicount", "3");
    wo.add("attr_dims", "all");

    try
    {
        writeOgr(wo, Support::datapath("las/simple.las"), "error.geojson");
        FAIL() << "Expected an exception for an invalid arg combination";
    }
    catch (pdal_error const & err)
    {
        EXPECT_EQ("writers.ogr: multicount > 1 incompatible with attr_dims",
            std::string(err.what()));
    }
}

TEST(OGRWriterTest, error_unknown_attr)
{
    Options wo;
    wo.add("ogrdriver", "GeoJSON");
    wo.add("attr_dims", "bananas");

    try
    {
        writeOgr(wo, Support::datapath("las/simple.las"), "error.geojson");
        FAIL() << "Expected an exception for an invalid arg combination";
    }
    catch (pdal_error const & err)
    {
        EXPECT_EQ("writers.ogr: Dimension 'bananas' (attr_dims) not found.",
            std::string(err.what()));
    }
}

TEST(OGRWriterTest, error_ogr)
{
    Options wo;
    wo.add("ogrdriver", "GeoJSON");

    // RFC7946 GeoJSON doesn't allow non-WGS84 CRS, and will throw an error
    wo.add("ogr_options", "RFC7946=YES");

    try
    {
        {
            gdal::ErrorHandlerSuspender devnull;

            writeOgr(wo, Support::datapath("las/simple.las"), "error.geojson");
        }
        FAIL() << "Expected an exception from OGR";
    }
    catch (pdal_error const & err)
    {
        EXPECT_EQ("writers.ogr: Can't create OGR layer: Failed to create coordinate transformation between the input coordinate system and WGS84.",
            std::string(err.what()));
    }
}

} // namespace pdal
