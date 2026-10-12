//! The manifest, which the frontend writes at elaboration: each event with its subject, lifecycle, and fields, each
//! enum, and each site.

use std::collections::HashMap;
use std::path::Path;

use indexmap::IndexMap;
use serde::Deserialize;
use sha2::{Digest, Sha256};

use crate::{Error, Result};

#[derive(Debug, Deserialize)]
pub struct Manifest {
    pub events: Vec<EventSchema>,
    pub enums: Vec<EnumSchema>,
    pub sites: Vec<Site>,

    /// SHA-256 of the manifest's bytes, in hex, which a trace's header must carry.
    #[serde(skip)]
    pub sha256: String,
    /// Each site's event, as an index into `events`, by site ID.
    #[serde(skip)]
    site_events: Vec<Option<usize>>,
    /// Each enum's variant names by value.
    #[serde(skip)]
    variants: HashMap<String, HashMap<u64, String>>,
}

#[derive(Debug, Deserialize)]
pub struct EventSchema {
    pub name: String,
    /// The object type of the event's subject, if it has one.
    pub subject: Option<String>,
    pub lifecycle: Option<Lifecycle>,
    pub fields: Vec<FieldSchema>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Lifecycle {
    Create,
    Destroy,
}

#[derive(Debug, Deserialize)]
pub struct FieldSchema {
    pub name: String,
    #[serde(rename = "type")]
    pub ty: FieldType,
    /// Bits, for every type but Id.
    pub width: Option<u32>,
    /// An Enum field's enum.
    #[serde(rename = "enum")]
    pub enum_name: Option<String>,
    /// An Id field's object type.
    pub object: Option<String>,
    /// Whether some site leaves the field DontCare.
    #[serde(default)]
    pub optional: bool,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Deserialize)]
pub enum FieldType {
    Bool,
    UInt,
    SInt,
    Enum,
    Id,
}

#[derive(Debug, Deserialize)]
pub struct EnumSchema {
    pub name: String,
    /// Each variant's value, in declaration order.
    pub values: IndexMap<String, u64>,
}

#[derive(Debug, Deserialize)]
pub struct Site {
    pub site_id: u32,
    pub event_name: String,
    pub instance: String,
    pub source: String,
}

impl Manifest {
    pub fn load(path: &Path) -> Result<Manifest> {
        let bytes = std::fs::read(path).map_err(|e| Error(format!("cannot read {}: {e}", path.display())))?;
        let mut manifest: Manifest = serde_json::from_slice(&bytes)
            .map_err(|e| Error(format!("{} is not a manifest: {e}", path.display())))?;
        manifest.sha256 = Sha256::digest(&bytes).iter().map(|b| format!("{b:02x}")).collect();

        let by_name: HashMap<&str, usize> = manifest.events.iter().enumerate().map(|(i, e)| (e.name.as_str(), i)).collect();
        let mut site_events = Vec::new();
        for site in &manifest.sites {
            let event = *by_name.get(site.event_name.as_str()).ok_or_else(|| {
                Error(format!("{}: site {} emits {}, which it does not list", path.display(), site.site_id, site.event_name))
            })?;
            let id = site.site_id as usize;
            if site_events.len() <= id {
                site_events.resize(id + 1, None);
            }
            site_events[id] = Some(event);
        }
        manifest.site_events = site_events;
        manifest.variants = manifest
            .enums
            .iter()
            .map(|e| (e.name.clone(), e.values.iter().map(|(name, &value)| (value, name.clone())).collect()))
            .collect();
        Ok(manifest)
    }

    /// The event a site emits, as an index into `events`.
    pub fn site_event(&self, site_id: u32) -> Option<usize> {
        self.site_events.get(site_id as usize).copied().flatten()
    }

    /// The name of an enum's variant with this value.
    pub fn variant(&self, enum_name: &str, value: u64) -> Option<&str> {
        self.variants.get(enum_name)?.get(&value).map(String::as_str)
    }

    /// The position of an event's field, in the manifest's order.
    pub fn field_index(&self, event: usize, field: &str) -> Option<usize> {
        self.events[event].fields.iter().position(|f| f.name == field)
    }
}
