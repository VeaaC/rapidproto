//! A serde `Serializer` that renders a decoded RapidProto message as ONE line of JSON, in the exact
//! shape the differential test's Python side (`tests/differential.py`) canonicalizes against the
//! protobuf descriptor and compares to the oracle. It is the Rust pendant of the C++ debug dumper's
//! comparison surface.
//!
//! The contract (see `canon_dump`/`canon_dump_scalar` in differential.py): a JSON object keyed by
//! proto field names; integers as bare digits; floats as a shortest round-trip decimal, or the quoted
//! strings `"NaN"`/`"Infinity"`/`"-Infinity"` for non-finite (JSON has no literal for them); `bytes`
//! as a quoted lowercase-hex string; an open enum as its bare `i32` (the oracle compares enums as
//! numbers); strings quoted and JSON-escaped; repeated as arrays; maps as objects whose keys are
//! quoted strings (so integer/bool keys round-trip). serde_json can't do this (it turns NaN/Inf into
//! `null` and rejects non-string map keys), which is why this is a bespoke serializer.
//!
//! Not for shipping: it only formats, is dev/test-only, and makes no promises beyond this contract.

use std::fmt::{self, Display, Write};

use serde::ser::{
    self, Serialize, SerializeMap, SerializeSeq, SerializeStruct, SerializeStructVariant,
    SerializeTuple, SerializeTupleStruct, SerializeTupleVariant, Serializer,
};

/// Render `value` as one line of the dump-JSON contract (no trailing newline).
///
/// # Errors
/// Returns an error only if the value's `Serialize` impl reports one; the generated impls do not.
pub fn to_line<T: Serialize + ?Sized>(value: &T) -> std::result::Result<String, Error> {
    let mut out = String::new();
    value.serialize(JsonSerializer { out: &mut out })?;
    Ok(out)
}

/// A serialization failure. The generated impls never produce one; this exists to satisfy serde.
#[derive(Debug)]
pub struct Error(String);

impl Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.0)
    }
}

impl std::error::Error for Error {}

impl ser::Error for Error {
    fn custom<T: Display>(msg: T) -> Self {
        Error(msg.to_string())
    }
}

type Result<T> = std::result::Result<T, Error>;

fn io(_e: fmt::Error) -> Error {
    Error("formatting into the output string failed".to_owned())
}

/// Append `s` JSON-escaped and quoted. Produces valid JSON; the Python side compares the decoded
/// string value, so only validity (not byte-identical escaping) matters.
fn write_quoted(out: &mut String, s: &str) -> Result<()> {
    out.push('"');
    for ch in s.chars() {
        match ch {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if (c as u32) < 0x20 => write!(out, "\\u{:04x}", c as u32).map_err(io)?,
            c => out.push(c),
        }
    }
    out.push('"');
    Ok(())
}

/// The root serializer and the per-element serializer (writes one JSON value into `out`).
struct JsonSerializer<'a> {
    out: &'a mut String,
}

impl<'a> Serializer for JsonSerializer<'a> {
    type Ok = ();
    type Error = Error;
    type SerializeSeq = SeqWriter<'a>;
    type SerializeTuple = SeqWriter<'a>;
    type SerializeTupleStruct = SeqWriter<'a>;
    type SerializeTupleVariant = SeqWriter<'a>;
    type SerializeMap = MapWriter<'a>;
    type SerializeStruct = MapWriter<'a>;
    type SerializeStructVariant = MapWriter<'a>;

    fn serialize_bool(self, v: bool) -> Result<()> {
        self.out.push_str(if v { "true" } else { "false" });
        Ok(())
    }
    fn serialize_i8(self, v: i8) -> Result<()> {
        write!(self.out, "{v}").map_err(io)
    }
    fn serialize_i16(self, v: i16) -> Result<()> {
        write!(self.out, "{v}").map_err(io)
    }
    fn serialize_i32(self, v: i32) -> Result<()> {
        write!(self.out, "{v}").map_err(io)
    }
    fn serialize_i64(self, v: i64) -> Result<()> {
        write!(self.out, "{v}").map_err(io)
    }
    fn serialize_i128(self, v: i128) -> Result<()> {
        write!(self.out, "{v}").map_err(io)
    }
    fn serialize_u8(self, v: u8) -> Result<()> {
        write!(self.out, "{v}").map_err(io)
    }
    fn serialize_u16(self, v: u16) -> Result<()> {
        write!(self.out, "{v}").map_err(io)
    }
    fn serialize_u32(self, v: u32) -> Result<()> {
        write!(self.out, "{v}").map_err(io)
    }
    fn serialize_u64(self, v: u64) -> Result<()> {
        write!(self.out, "{v}").map_err(io)
    }
    fn serialize_u128(self, v: u128) -> Result<()> {
        write!(self.out, "{v}").map_err(io)
    }
    fn serialize_f32(self, v: f32) -> Result<()> {
        write_float(
            self.out,
            f64::from(v),
            v.is_finite(),
            v.is_nan(),
            v.is_sign_positive(),
        )
    }
    fn serialize_f64(self, v: f64) -> Result<()> {
        write_float(self.out, v, v.is_finite(), v.is_nan(), v.is_sign_positive())
    }
    fn serialize_char(self, v: char) -> Result<()> {
        let mut buf = [0u8; 4];
        write_quoted(self.out, v.encode_utf8(&mut buf))
    }
    fn serialize_str(self, v: &str) -> Result<()> {
        write_quoted(self.out, v)
    }
    fn serialize_bytes(self, v: &[u8]) -> Result<()> {
        self.out.push('"');
        for b in v {
            write!(self.out, "{b:02x}").map_err(io)?;
        }
        self.out.push('"');
        Ok(())
    }
    fn serialize_none(self) -> Result<()> {
        self.out.push_str("null");
        Ok(())
    }
    fn serialize_some<T: Serialize + ?Sized>(self, value: &T) -> Result<()> {
        value.serialize(self)
    }
    fn serialize_unit(self) -> Result<()> {
        self.out.push_str("null");
        Ok(())
    }
    fn serialize_unit_struct(self, _name: &'static str) -> Result<()> {
        self.serialize_unit()
    }
    fn serialize_unit_variant(
        self,
        _n: &'static str,
        _i: u32,
        variant: &'static str,
    ) -> Result<()> {
        write_quoted(self.out, variant)
    }
    fn serialize_newtype_struct<T: Serialize + ?Sized>(
        self,
        _n: &'static str,
        v: &T,
    ) -> Result<()> {
        v.serialize(self)
    }
    fn serialize_newtype_variant<T: Serialize + ?Sized>(
        self,
        _n: &'static str,
        _i: u32,
        variant: &'static str,
        value: &T,
    ) -> Result<()> {
        // `{ "<variant>": <value> }`
        let mut m = self.serialize_map(Some(1))?;
        SerializeMap::serialize_entry(&mut m, variant, value)?;
        SerializeMap::end(m)
    }
    fn serialize_seq(self, _len: Option<usize>) -> Result<SeqWriter<'a>> {
        self.out.push('[');
        Ok(SeqWriter {
            out: self.out,
            first: true,
        })
    }
    fn serialize_tuple(self, len: usize) -> Result<SeqWriter<'a>> {
        self.serialize_seq(Some(len))
    }
    fn serialize_tuple_struct(self, _n: &'static str, len: usize) -> Result<SeqWriter<'a>> {
        self.serialize_seq(Some(len))
    }
    fn serialize_tuple_variant(
        self,
        _n: &'static str,
        _i: u32,
        variant: &'static str,
        _len: usize,
    ) -> Result<SeqWriter<'a>> {
        // `{ "<variant>": [ ... ] }`
        write!(self.out, "{{\"{variant}\":").map_err(io)?;
        self.out.push('[');
        Ok(SeqWriter {
            out: self.out,
            first: true,
        })
    }
    fn serialize_map(self, _len: Option<usize>) -> Result<MapWriter<'a>> {
        self.out.push('{');
        Ok(MapWriter {
            out: self.out,
            first: true,
            close_variant: false,
        })
    }
    fn serialize_struct(self, _n: &'static str, len: usize) -> Result<MapWriter<'a>> {
        self.serialize_map(Some(len))
    }
    fn serialize_struct_variant(
        self,
        _n: &'static str,
        _i: u32,
        variant: &'static str,
        _len: usize,
    ) -> Result<MapWriter<'a>> {
        // `{ "<variant>": { ... } }`
        write!(self.out, "{{\"{variant}\":").map_err(io)?;
        self.out.push('{');
        Ok(MapWriter {
            out: self.out,
            first: true,
            close_variant: true,
        })
    }
}

fn write_float(out: &mut String, v: f64, finite: bool, nan: bool, positive: bool) -> Result<()> {
    if finite {
        // Rust's `{}` is a shortest round-trip decimal (no exponent form), valid JSON that Python's
        // float() recovers exactly -- which is all the bit-pattern comparison needs.
        write!(out, "{v}").map_err(io)
    } else {
        out.push_str(if nan {
            "\"NaN\""
        } else if positive {
            "\"Infinity\""
        } else {
            "\"-Infinity\""
        });
        Ok(())
    }
}

struct SeqWriter<'a> {
    out: &'a mut String,
    first: bool,
}

impl SeqWriter<'_> {
    fn element<T: Serialize + ?Sized>(&mut self, value: &T) -> Result<()> {
        if !self.first {
            self.out.push(',');
        }
        self.first = false;
        value.serialize(JsonSerializer { out: self.out })
    }
}

impl SerializeSeq for SeqWriter<'_> {
    type Ok = ();
    type Error = Error;
    fn serialize_element<T: Serialize + ?Sized>(&mut self, value: &T) -> Result<()> {
        self.element(value)
    }
    fn end(self) -> Result<()> {
        self.out.push(']');
        Ok(())
    }
}

impl SerializeTuple for SeqWriter<'_> {
    type Ok = ();
    type Error = Error;
    fn serialize_element<T: Serialize + ?Sized>(&mut self, value: &T) -> Result<()> {
        self.element(value)
    }
    fn end(self) -> Result<()> {
        self.out.push(']');
        Ok(())
    }
}

impl SerializeTupleStruct for SeqWriter<'_> {
    type Ok = ();
    type Error = Error;
    fn serialize_field<T: Serialize + ?Sized>(&mut self, value: &T) -> Result<()> {
        self.element(value)
    }
    fn end(self) -> Result<()> {
        self.out.push(']');
        Ok(())
    }
}

impl SerializeTupleVariant for SeqWriter<'_> {
    type Ok = ();
    type Error = Error;
    fn serialize_field<T: Serialize + ?Sized>(&mut self, value: &T) -> Result<()> {
        self.element(value)
    }
    fn end(self) -> Result<()> {
        self.out.push_str("]}");
        Ok(())
    }
}

struct MapWriter<'a> {
    out: &'a mut String,
    first: bool,
    close_variant: bool,
}

impl MapWriter<'_> {
    fn comma(&mut self) {
        if !self.first {
            self.out.push(',');
        }
        self.first = false;
    }
}

impl SerializeMap for MapWriter<'_> {
    type Ok = ();
    type Error = Error;
    fn serialize_key<T: Serialize + ?Sized>(&mut self, key: &T) -> Result<()> {
        self.comma();
        key.serialize(KeySerializer { out: self.out })
    }
    fn serialize_value<T: Serialize + ?Sized>(&mut self, value: &T) -> Result<()> {
        self.out.push(':');
        value.serialize(JsonSerializer { out: self.out })
    }
    fn serialize_entry<K: Serialize + ?Sized, V: Serialize + ?Sized>(
        &mut self,
        key: &K,
        value: &V,
    ) -> Result<()> {
        self.comma();
        key.serialize(KeySerializer { out: self.out })?;
        self.out.push(':');
        value.serialize(JsonSerializer { out: self.out })
    }
    fn end(self) -> Result<()> {
        self.out.push('}');
        if self.close_variant {
            self.out.push('}');
        }
        Ok(())
    }
}

impl SerializeStruct for MapWriter<'_> {
    type Ok = ();
    type Error = Error;
    fn serialize_field<T: Serialize + ?Sized>(
        &mut self,
        key: &'static str,
        value: &T,
    ) -> Result<()> {
        self.comma();
        write_quoted(self.out, key)?;
        self.out.push(':');
        value.serialize(JsonSerializer { out: self.out })
    }
    fn end(self) -> Result<()> {
        self.out.push('}');
        Ok(())
    }
}

impl SerializeStructVariant for MapWriter<'_> {
    type Ok = ();
    type Error = Error;
    fn serialize_field<T: Serialize + ?Sized>(
        &mut self,
        key: &'static str,
        value: &T,
    ) -> Result<()> {
        self.comma();
        write_quoted(self.out, key)?;
        self.out.push(':');
        value.serialize(JsonSerializer { out: self.out })
    }
    fn end(self) -> Result<()> {
        self.out.push_str("}}");
        Ok(())
    }
}

/// Serializes a map KEY as a quoted JSON string, whatever its type -- so integer and bool keys become
/// `"<n>"` / `"true"`, matching the dump contract (JSON object keys are strings). Only the scalar key
/// kinds protobuf maps allow are supported.
struct KeySerializer<'a> {
    out: &'a mut String,
}

impl KeySerializer<'_> {
    fn quoted_num<T: Display>(self, v: T) -> Result<()> {
        write!(self.out, "\"{v}\"").map_err(io)
    }
}

impl Serializer for KeySerializer<'_> {
    type Ok = ();
    type Error = Error;
    type SerializeSeq = ser::Impossible<(), Error>;
    type SerializeTuple = ser::Impossible<(), Error>;
    type SerializeTupleStruct = ser::Impossible<(), Error>;
    type SerializeTupleVariant = ser::Impossible<(), Error>;
    type SerializeMap = ser::Impossible<(), Error>;
    type SerializeStruct = ser::Impossible<(), Error>;
    type SerializeStructVariant = ser::Impossible<(), Error>;

    fn serialize_bool(self, v: bool) -> Result<()> {
        self.out.push_str(if v { "\"true\"" } else { "\"false\"" });
        Ok(())
    }
    fn serialize_i8(self, v: i8) -> Result<()> {
        self.quoted_num(v)
    }
    fn serialize_i16(self, v: i16) -> Result<()> {
        self.quoted_num(v)
    }
    fn serialize_i32(self, v: i32) -> Result<()> {
        self.quoted_num(v)
    }
    fn serialize_i64(self, v: i64) -> Result<()> {
        self.quoted_num(v)
    }
    fn serialize_i128(self, v: i128) -> Result<()> {
        self.quoted_num(v)
    }
    fn serialize_u8(self, v: u8) -> Result<()> {
        self.quoted_num(v)
    }
    fn serialize_u16(self, v: u16) -> Result<()> {
        self.quoted_num(v)
    }
    fn serialize_u32(self, v: u32) -> Result<()> {
        self.quoted_num(v)
    }
    fn serialize_u64(self, v: u64) -> Result<()> {
        self.quoted_num(v)
    }
    fn serialize_u128(self, v: u128) -> Result<()> {
        self.quoted_num(v)
    }
    fn serialize_str(self, v: &str) -> Result<()> {
        write_quoted(self.out, v)
    }
    fn serialize_char(self, v: char) -> Result<()> {
        let mut buf = [0u8; 4];
        write_quoted(self.out, v.encode_utf8(&mut buf))
    }
    fn serialize_bytes(self, _v: &[u8]) -> Result<()> {
        Err(Error("map key cannot be bytes".to_owned()))
    }
    fn serialize_f32(self, _v: f32) -> Result<()> {
        Err(Error("map key cannot be a float".to_owned()))
    }
    fn serialize_f64(self, _v: f64) -> Result<()> {
        Err(Error("map key cannot be a float".to_owned()))
    }
    fn serialize_none(self) -> Result<()> {
        Err(Error("map key cannot be none".to_owned()))
    }
    fn serialize_some<T: Serialize + ?Sized>(self, _v: &T) -> Result<()> {
        Err(Error("map key cannot be an option".to_owned()))
    }
    fn serialize_unit(self) -> Result<()> {
        Err(Error("map key cannot be unit".to_owned()))
    }
    fn serialize_unit_struct(self, _n: &'static str) -> Result<()> {
        Err(Error("map key cannot be a unit struct".to_owned()))
    }
    fn serialize_unit_variant(self, _n: &'static str, _i: u32, v: &'static str) -> Result<()> {
        write_quoted(self.out, v)
    }
    fn serialize_newtype_struct<T: Serialize + ?Sized>(
        self,
        _n: &'static str,
        v: &T,
    ) -> Result<()> {
        v.serialize(self)
    }
    fn serialize_newtype_variant<T: Serialize + ?Sized>(
        self,
        _n: &'static str,
        _i: u32,
        _v: &'static str,
        _val: &T,
    ) -> Result<()> {
        Err(Error("map key cannot be a newtype variant".to_owned()))
    }
    fn serialize_seq(self, _len: Option<usize>) -> Result<Self::SerializeSeq> {
        Err(Error("map key cannot be a sequence".to_owned()))
    }
    fn serialize_tuple(self, _len: usize) -> Result<Self::SerializeTuple> {
        Err(Error("map key cannot be a tuple".to_owned()))
    }
    fn serialize_tuple_struct(
        self,
        _n: &'static str,
        _len: usize,
    ) -> Result<Self::SerializeTupleStruct> {
        Err(Error("map key cannot be a tuple struct".to_owned()))
    }
    fn serialize_tuple_variant(
        self,
        _n: &'static str,
        _i: u32,
        _v: &'static str,
        _len: usize,
    ) -> Result<Self::SerializeTupleVariant> {
        Err(Error("map key cannot be a tuple variant".to_owned()))
    }
    fn serialize_map(self, _len: Option<usize>) -> Result<Self::SerializeMap> {
        Err(Error("map key cannot be a map".to_owned()))
    }
    fn serialize_struct(self, _n: &'static str, _len: usize) -> Result<Self::SerializeStruct> {
        Err(Error("map key cannot be a struct".to_owned()))
    }
    fn serialize_struct_variant(
        self,
        _n: &'static str,
        _i: u32,
        _v: &'static str,
        _len: usize,
    ) -> Result<Self::SerializeStructVariant> {
        Err(Error("map key cannot be a struct variant".to_owned()))
    }
}
