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
#include <filters/RangeFilter.hpp>
#include <filters/SortFilter.hpp>
#include <io/BufferReader.hpp>
#include <io/FauxReader.hpp>
#include <io/OGRWriter.hpp>
#include <io/LasReader.hpp>
#include "Support.hpp"

#include <gdal_priv.h>
#include <ogrsf_frmts.h>

#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace pdal
{

namespace
{

// Returns the path written
std::string runOgrWriterInfo(const Options& wo, const std::string& infile,
    const std::string& infofile, const std::string suffix, int featureCount = 10,
    uint32_t(*compare)(const std::string&, const std::string&, int32_t) = &Support::diff_text_files
)
{
    FileUtils::createDirectory(Support::temppath("ogr"));
    std::string outfile(Support::temppath(std::string("ogr/") + FileUtils::stem(FileUtils::stem(FileUtils::getFilename(infofile))) + suffix));
    std::string outinfofile(outfile + ".ogrinfo");

    if(FileUtils::isDirectory(outfile)) {
        FileUtils::deleteDirectory(outfile);
    } else {
        FileUtils::deleteFile(outfile);
    }
    FileUtils::deleteFile(outinfofile);

    Options readerOpts;
    readerOpts.add("filename", infile);

    LasReader reader;
    reader.setOptions(readerOpts);

    Options sortOpts;
    SortFilter sortFilter;
    sortOpts.add("dimension", "X");
    sortFilter.setOptions(sortOpts);
    sortFilter.setInput(reader);

    Options writerOpts;
    writerOpts.add("filename", outfile);

    OGRWriter writer;
    writer.setOptions(wo);
    writer.addOptions(writerOpts);

    HeadFilter headFilter;
    if (featureCount > 0) {
        Options headOpts;
        headOpts.add("count", featureCount);
        headFilter.setOptions(headOpts);
        headFilter.setInput(sortFilter);
        writer.setInput(headFilter);
    } else {
        writer.setInput(sortFilter);
    }

    PointTable t;
    writer.prepare(t);
    writer.execute(t);

    std::string cmd = "ogrinfo -nomd -al " + outfile;
    if (featureCount == 0) {
        cmd += " -so";
    }
    cmd += " 2>&1";
    std::string info;
    if (Utils::run_shell_command(cmd, info)) {
        std::cerr << "WARNING: error running ogrinfo, skipping test" << std::endl;
        return outfile;
    }

    auto handle = FileUtils::createFile(outinfofile);
    *handle << info;
    FileUtils::closeFile(handle);

    EXPECT_EQ(compare(infofile, outinfofile, 1), 0u) \
        << "  (" << infofile << " <-> " << outinfofile << ")";
    return outfile;
}

uint32_t diff_geojson(const std::string& file1, const std::string& file2, int32_t ignoreLine1=-1)
{
    // GeoJSON files depend on PROJ version, later versions write a much different
    // SRS format. Crudely ignore lines between ^GEOGCRS[ and ^OGRFeature

    if (!Utils::fileExists(file1) || !Utils::fileExists(file2))
        return (std::numeric_limits<uint32_t>::max)();
    if (!Utils::fileExists(file1) || !Utils::fileExists(file2))
        return (std::numeric_limits<uint32_t>::max)();

    std::istream *istr1 = Utils::openFile(file1, false);
    std::istream *istr2 = Utils::openFile(file2, false);
    std::istream& str1 = *istr1;
    std::istream& str2 = *istr2;

    uint32_t numdiffs = 0;
    int32_t currLine = 1;
    bool buf1ignore = false;
    bool buf2ignore = false;
    std::string buf1;
    std::string buf2;

    while (!str1.eof() && !str2.eof())
    {
        if (currLine == ignoreLine1)
        {
            std::getline(str1, buf1);
            std::getline(str2, buf2);
            ++currLine;
            continue;
        }

        if (!(buf1ignore ^ buf2ignore))
        {
            std::getline(str1, buf1);
            std::getline(str2, buf2);
        }
        else if (!buf2ignore)
            std::getline(str1, buf1);
        else if (!buf1ignore)
            std::getline(str2, buf2);

        // EOF handling won't work properly if we're in an ignored block. NBD
        if (str1.eof() && str2.eof())
        {
            // hit end on both together
            break;
        }
        else if (str1.eof() && !str2.eof())
        {
            // str1 ended, but str2 still going
            while (!str2.eof())
            {
                std::getline(str2, buf2);
                ++numdiffs;
            }
            break;
        }
        else if (!str1.eof() && str2.eof())
        {
            // str2 ended, but str1 still going
            while (!str1.eof())
            {
                std::getline(str1, buf1);
                ++numdiffs;
            }
            break;
        }

        if (buf1ignore && buf1.rfind("OGRFeature", 0) == 0)
            buf1ignore = false;
        else if (buf1.rfind("GEOGCRS[", 0) == 0)
            buf1ignore = true;

        if (buf2ignore && buf2.rfind("OGRFeature", 0) == 0)
            buf2ignore = false;
        else if (buf2.rfind("GEOGCRS[", 0) == 0)
            buf2ignore = true;

        // remove line endings from test comparisons
        buf1.erase(std::remove(buf1.begin(), buf1.end(), '\r' ), buf1.end());
        buf1.erase(std::remove(buf1.begin(), buf1.end(), '\n' ), buf1.end());
        buf2.erase(std::remove(buf2.begin(), buf2.end(), '\r' ), buf2.end());
        buf2.erase(std::remove(buf2.begin(), buf2.end(), '\n' ), buf2.end());

        if (!buf1ignore && !buf2ignore && buf1 != buf2)
            ++numdiffs;

        ++currLine;
    }

    assert(str1.eof());
    assert(str2.eof());

    Utils::closeFile(istr1);
    Utils::closeFile(istr2);

    return numdiffs;
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
    std::string infile = Support::datapath("las/simple.las");
    std::string infofile = Support::datapath("ogr/shapefile.shp.ogrinfo");

    Options wo;
    wo.add("ogrdriver", "ESRI Shapefile");

    std::string out = runOgrWriterInfo(wo, infile, infofile, ".shp");
    checkLayer(out, { wkbPoint25D, 10,
        envelope(635619.85, 849017.32, 635685.33, 852165.12),
        { 635619.85, 850064.04, 447.01 },
        {},
        {},
        "" });
}

TEST(OGRWriterTest, json)
{
    std::string infile = Support::datapath("las/simple.las");
    std::string infofile = Support::datapath("ogr/json.geojson.ogrinfo");

    Options wo;
    wo.add("ogrdriver", "GeoJSON");

    std::string out = runOgrWriterInfo(wo, infile, infofile, ".geojson", 10,
        &diff_geojson);
    checkLayer(out, { wkbPoint25D, 10,
        envelope(635619.85, 849017.32, 635685.33, 852165.12),
        { 635619.85, 850064.04, 447.01 },
        {},
        {},
        "WGS 84" });
}

TEST(OGRWriterTest, geopackage)
{
    std::string infile = Support::datapath("las/utm15.las");
    std::string infofile = Support::datapath("ogr/geopackage.gpkg.ogrinfo");

    Options wo;
    wo.add("ogrdriver", "GPKG");

    std::string out = runOgrWriterInfo(wo, infile, infofile, ".gpkg");
    checkLayer(out, { wkbPointZM, 1,
        envelope(470692.44, 4602888.9, 470692.44, 4602888.9),
        { 470692.44, 4602888.9, 16 },
        {},
        {},
        "NAD83 / UTM zone 15N" });
}

TEST(OGRWriterTest, creation_options)
{
    std::string infile = Support::datapath("las/simple.las");
    #if GDAL_VERSION_NUM >= GDAL_COMPUTE_VERSION(3,9,0)
        std::string infofile = Support::datapath("ogr/creation_options_gdal_gt_390.geojson.ogrinfo");
    #else
        std::string infofile = Support::datapath("ogr/creation_options.geojson.ogrinfo");
    #endif

        Options wo;
        wo.add("ogrdriver", "GeoJSON");
        wo.add("ogr_options", "WRITE_BBOX=YES");
        wo.add("ogr_options", "COORDINATE_PRECISION=1");

        std::string out = runOgrWriterInfo(wo, infile, infofile, ".geojson",
            10, &diff_geojson);

    // both .ogrinfo files give these values
    checkLayer(out, { wkbPoint25D, 10,
        envelope(635619.8, 849017.3, 635685.3, 852165.1),
        { 635619.8, 850064, 447 },
        {},
        {},
        "WGS 84" });
}

TEST(OGRWriterTest, shapefile_measure)
{
    std::string infile = Support::datapath("las/simple.las");
    std::string infofile = Support::datapath("ogr/shapefile_measure.shp.ogrinfo");

    Options wo;
    wo.add("ogrdriver", "ESRI Shapefile");
    wo.add("measure_dim", "Classification");

    std::string out = runOgrWriterInfo(wo, infile, infofile, ".shp");
    checkLayer(out, { wkbPointZM, 10,
        envelope(635619.85, 849017.32, 635685.33, 852165.12),
        { 635619.85, 850064.04, 447.01, 1 },
        {},
        {},
        "" });
}

TEST(OGRWriterTest, attrs_all)
{
    std::string infile = Support::datapath("las/simple.las");
    std::string infofile = Support::datapath("ogr/attrs_all.shp.ogrinfo");

    Options wo;
    wo.add("ogrdriver", "ESRI Shapefile");
    wo.add("attr_dims", "all");

    std::string out = runOgrWriterInfo(wo, infile, infofile, ".shp");
    checkLayer(out, { wkbPoint25D, 10,
        envelope(635619.85, 849017.32, 635685.33, 852165.12),
        { 635619.85, 850064.04, 447.01 },
        { { "Intensity", OFTInteger, 1 },
          { "ReturnNumb", OFTInteger, 1 },
          { "NumberOfRe", OFTInteger, 2 },
          { "ScanDirect", OFTInteger, 0 },
          { "EdgeOfFlig", OFTInteger, 0 },
          { "Classifica", OFTInteger, 1 },
          { "Synthetic", OFTInteger, 0 },
          { "KeyPoint", OFTInteger, 0 },
          { "Withheld", OFTInteger, 0 },
          { "Overlap", OFTInteger, 0 },
          { "ScanAngleR", OFTReal, 15 },
          { "UserData", OFTInteger, 125 },
          { "PointSourc", OFTInteger, 7327 },
          { "GpsTime", OFTReal, 246092.20788099366473 },
          { "Red", OFTInteger, 52 },
          { "Green", OFTInteger, 65 },
          { "Blue", OFTInteger, 68 } },
        {},
        "" });
}

TEST(OGRWriterTest, geopackage_attrs_all)
{
    std::string infile = Support::datapath("las/simple.las");

    #if GDAL_VERSION_NUM >= GDAL_COMPUTE_VERSION(3,9,0)
        std::string infofile = Support::datapath("ogr/geopackage_attrs_all_gdal_gt_390.gpkg.ogrinfo");
    #else
        std::string infofile = Support::datapath("ogr/geopackage_attrs_all.gpkg.ogrinfo");
    #endif


    Options wo;
    wo.add("ogrdriver", "GPKG");
    wo.add("attr_dims", "all");

    std::string out = runOgrWriterInfo(wo, infile, infofile, ".gpkg");
#if GDAL_VERSION_NUM >= GDAL_COMPUTE_VERSION(3,9,0)
    checkLayer(out, { wkbPointZM, 10,
        envelope(635619.85, 849017.32, 635685.33, 852165.12),
        { 635619.85, 850064.04, 447.01 },
        { { "Intensity", OFTInteger, 1 },
          { "ReturnNumber", OFTInteger, 1 },
          { "NumberOfReturns", OFTInteger, 2 },
          { "ScanDirectionFlag", OFTInteger, 0 },
          { "EdgeOfFlightLine", OFTInteger, 0 },
          { "Classification", OFTInteger, 1 },
          { "Synthetic", OFTInteger, 0 },
          { "KeyPoint", OFTInteger, 0 },
          { "Withheld", OFTInteger, 0 },
          { "Overlap", OFTInteger, 0 },
          { "ScanAngleRank", OFTReal, 15 },
          { "UserData", OFTInteger, 125 },
          { "PointSourceId", OFTInteger, 7327 },
          { "GpsTime", OFTReal, 246092.207880994 },
          { "Red", OFTInteger, 52 },
          { "Green", OFTInteger, 65 },
          { "Blue", OFTInteger, 68 } },
        {},
        "" });
#else
    checkLayer(out, { wkbPointZM, 10,
        envelope(635619.85, 849017.32, 635685.33, 852165.12),
        { 635619.85, 850064.04, 447.01 },
        { { "Intensity", OFTInteger, 1 },
          { "ReturnNumber", OFTInteger, 1 },
          { "NumberOfReturns", OFTInteger, 2 },
          { "ScanDirectionFlag", OFTInteger, 0 },
          { "EdgeOfFlightLine", OFTInteger, 0 },
          { "Classification", OFTInteger, 1 },
          { "Synthetic", OFTInteger, 0 },
          { "KeyPoint", OFTInteger, 0 },
          { "Withheld", OFTInteger, 0 },
          { "Overlap", OFTInteger, 0 },
          { "ScanAngleRank", OFTReal, 15 },
          { "UserData", OFTInteger, 125 },
          { "PointSourceId", OFTInteger, 7327 },
          { "GpsTime", OFTReal, 246092.207880994 },
          { "Red", OFTInteger, 52 },
          { "Green", OFTInteger, 65 },
          { "Blue", OFTInteger, 68 } },
        {},
        "Undefined geographic SRS" });
#endif
}

TEST(OGRWriterTest, attrs)
{
    std::string infile = Support::datapath("las/simple.las");
    std::string infofile = Support::datapath("ogr/attrs.shp.ogrinfo");

    Options wo;
    wo.add("ogrdriver", "ESRI Shapefile");
    wo.add("attr_dims", "Classification,Red,Green,Blue");

    std::string out = runOgrWriterInfo(wo, infile, infofile, ".shp");
    checkLayer(out, { wkbPoint25D, 10,
        envelope(635619.85, 849017.32, 635685.33, 852165.12),
        { 635619.85, 850064.04, 447.01 },
        { { "Classifica", OFTInteger, 1 },
          { "Red", OFTInteger, 52 },
          { "Green", OFTInteger, 65 },
          { "Blue", OFTInteger, 68 } },
        {},
        "" });
}

TEST(OGRWriterTest, attrs_measure)
{
    std::string infile = Support::datapath("las/simple.las");
    std::string infofile = Support::datapath("ogr/attrs_measure.shp.ogrinfo");

    Options wo;
    wo.add("ogrdriver", "ESRI Shapefile");
    wo.add("measure_dim", "Classification");
    wo.add("attr_dims", "all");

    std::string out = runOgrWriterInfo(wo, infile, infofile, ".shp");
    checkLayer(out, { wkbPointZM, 10,
        envelope(635619.85, 849017.32, 635685.33, 852165.12),
        { 635619.85, 850064.04, 447.01, 1 },
        { { "Intensity", OFTInteger, 1 },
          { "ReturnNumb", OFTInteger, 1 },
          { "NumberOfRe", OFTInteger, 2 },
          { "ScanDirect", OFTInteger, 0 },
          { "EdgeOfFlig", OFTInteger, 0 },
          { "Synthetic", OFTInteger, 0 },
          { "KeyPoint", OFTInteger, 0 },
          { "Withheld", OFTInteger, 0 },
          { "Overlap", OFTInteger, 0 },
          { "ScanAngleR", OFTReal, 15 },
          { "UserData", OFTInteger, 125 },
          { "PointSourc", OFTInteger, 7327 },
          { "GpsTime", OFTReal, 246092.20788099366473 },
          { "Red", OFTInteger, 52 },
          { "Green", OFTInteger, 65 },
          { "Blue", OFTInteger, 68 } },
        {},
        "" });
}

TEST(OGRWriterTest, multicount)
{
    std::string infile = Support::datapath("las/simple.las");
    std::string infofile = Support::datapath("ogr/multicount.shp.ogrinfo");

    Options wo;
    wo.add("ogrdriver", "ESRI Shapefile");
    wo.add("multicount", "3");

    std::string out = runOgrWriterInfo(wo, infile, infofile, ".shp");
    checkLayer(out, { wkbMultiPoint25D, 4,
        envelope(635619.85, 849017.32, 635685.33, 852165.12),
        { 635619.85, 850064.04, 447.01 },
        {},
        { 3, 3, 3, 1 },
        "" });
}

TEST(OGRWriterTest, multicount_2)
{
    // feature count < multicount
    std::string infile = Support::datapath("las/simple.las");
    std::string infofile = Support::datapath("ogr/multicount_2.shp.ogrinfo");

    Options wo;
    wo.add("ogrdriver", "ESRI Shapefile");
    wo.add("multicount", "20");

    std::string out = runOgrWriterInfo(wo, infile, infofile, ".shp");
    checkLayer(out, { wkbMultiPoint25D, 1,
        envelope(635619.85, 849017.32, 635685.33, 852165.12),
        { 635619.85, 850064.04, 447.01 },
        {},
        { 10 },
        "" });
}

TEST(OGRWriterTest, multicount_3)
{
    // feature count == exact multiple of multicount
    std::string infile = Support::datapath("las/simple.las");
    std::string infofile = Support::datapath("ogr/multicount_3.shp.ogrinfo");

    Options wo;
    wo.add("ogrdriver", "ESRI Shapefile");
    wo.add("multicount", "5");

    std::string out = runOgrWriterInfo(wo, infile, infofile, ".shp");
    checkLayer(out, { wkbMultiPoint25D, 2,
        envelope(635619.85, 849017.32, 635685.33, 852165.12),
        { 635619.85, 850064.04, 447.01 },
        {},
        { 5, 5 },
        "" });
}

TEST(OGRWriterTest, error_multicount_attrs)
{
    std::string infile = Support::datapath("las/simple.las");
    std::string infofile = Support::datapath("ogr/error");

    Options wo;
    wo.add("ogrdriver", "GeoJSON");
    wo.add("multicount", "3");
    wo.add("attr_dims", "all");

    try
    {
        runOgrWriterInfo(wo, infile, infofile, ".geojson");
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
    std::string infile = Support::datapath("las/simple.las");
    std::string infofile = Support::datapath("ogr/error");

    Options wo;
    wo.add("ogrdriver", "GeoJSON");
    wo.add("attr_dims", "bananas");

    try
    {
        runOgrWriterInfo(wo, infile, infofile, ".geojson");
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
    std::string infile = Support::datapath("las/simple.las");
    std::string infofile = Support::datapath("ogr/error");

    Options wo;
    wo.add("ogrdriver", "GeoJSON");

    // RFC7946 GeoJSON doesn't allow non-WGS84 CRS, and will throw an error
    wo.add("ogr_options", "RFC7946=YES");

    try
    {
        {
            gdal::ErrorHandlerSuspender devnull;

            runOgrWriterInfo(wo, infile, infofile, ".geojson");
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
