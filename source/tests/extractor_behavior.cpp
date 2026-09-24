// Exercise the actual HDF5 dependency and the production extraction path.
#define wmain extractor_request_entry
#include "../cpp/extractor.cpp"
#undef wmain

void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

int main() {
    const fs::path root = fs::temp_directory_path() /
        ("sp_hdf_audit_" + std::to_string(GetCurrentProcessId()));
    try {
        // Never reuse an existing fixture directory.
        require(fs::create_directory(root), "fixture directory already exists");
        const fs::path fixedPath = root / "fixed.h5";
        const fs::path variablePath = root / "variable.h5";
        const std::string xml = "<root label=\"Archived HDF caption\"/>";
        {
            const HdfHandle file(H5Fcreate(pathUtf8(fixedPath).c_str(), H5F_ACC_EXCL,
                                          H5P_DEFAULT, H5P_DEFAULT), H5Fclose);
            const hsize_t length = xml.size();
            const HdfHandle space(H5Screate_simple(1, &length, nullptr), H5Sclose);
            const HdfHandle data(H5Dcreate2(file.id, "metadata.xml", H5T_NATIVE_UCHAR,
                space.id, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT), H5Dclose);
            require(H5Dwrite(data.id, H5T_NATIVE_UCHAR, H5S_ALL, H5S_ALL,
                              H5P_DEFAULT, xml.data()) >= 0, "write fixed HDF data");
        }
        {
            const HdfHandle variable(H5Tcopy(H5T_C_S1), H5Tclose);
            require(H5Tset_size(variable.id, H5T_VARIABLE) >= 0, "variable string type");
            require(hdfHasVariableStorage(variable.id), "reject variable strings");
            const HdfHandle compound(H5Tcreate(H5T_COMPOUND, sizeof(char *)), H5Tclose);
            require(H5Tinsert(compound.id, "name", 0, variable.id) >= 0, "compound fixture");
            require(hdfHasVariableStorage(compound.id), "reject nested variable strings");
            const hsize_t length = 2;
            const HdfHandle array(H5Tarray_create2(compound.id, 1, &length), H5Tclose);
            require(hdfHasVariableStorage(array.id), "reject array of variable compounds");
            require(!hdfHasVariableStorage(H5T_NATIVE_UCHAR), "allow raw resource bytes");
            const HdfHandle file(H5Fcreate(pathUtf8(variablePath).c_str(), H5F_ACC_EXCL,
                                          H5P_DEFAULT, H5P_DEFAULT), H5Fclose);
            const HdfHandle space(H5Screate(H5S_SCALAR), H5Sclose);
            const HdfHandle data(H5Dcreate2(file.id, "payload", variable.id, space.id,
                H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT), H5Dclose);
            const char *text = "Pointer-backed payload";
            require(H5Dwrite(data.id, variable.id, H5S_ALL, H5S_ALL,
                              H5P_DEFAULT, &text) >= 0, "write variable HDF data");
        }
        State state{};
        state.request.attributes.insert("label");
        const fs::path output = root / "output";
        fs::create_directory(output);
        const auto handles = H5Fget_obj_count(H5F_OBJ_ALL, H5F_OBJ_ALL);
        extractHdf5(state, fixedPath, output);
        parseXml(state, output / "metadata.xml");
        require(state.terms.count("Archived HDF caption") == 1, "preserve fixed HDF metadata extraction");
        bool rejected = false;
        try { extractHdf5(state, variablePath, output); }
        catch (const std::runtime_error &error) {
            rejected = std::string(error.what()).find("variable-length") != std::string::npos;
        }
        require(rejected, "reject pointer-backed payload before reading");
        require(H5Fget_obj_count(H5F_OBJ_ALL, H5F_OBJ_ALL) == handles,
                "HDF handles close on success and exceptions");
        require(!fs::exists(output / "payload"), "no invalid raw-pointer output");
        fs::remove(output / "metadata.xml");
        fs::remove(output);
        fs::remove(fixedPath);
        fs::remove(variablePath);
        fs::remove(root);
        std::cout << "Extractor HDF behavior passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
