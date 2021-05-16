#include <QDebug>
#include <QJsonDocument>
#include <QFile>
#include <QQmlEngine>
#include <QQmlContext>

#include <private/qmetaobjectbuilder_p.h>

#include "configengine.h"
#include "jsonqobject.h"

ConfigEngine::ConfigEngine(QObject *parent)
    : QObject(parent)
{
    m_data.resize(LevelsCount);
}

void ConfigEngine::setProperty(const QString &key, QVariant value, ConfigEngine::ConfigLevel level)
{
    QStringList parts = key.split(".");
    QString lastKey = parts.takeLast();


}

void ConfigEngine::loadData(const QByteArray &data, ConfigEngine::ConfigLevel level)
{
    QJsonParseError err;
    QJsonDocument json = QJsonDocument::fromJson(data, &err);
    if (err.error != QJsonParseError::NoError) {
        qWarning() << "Parse error" << err.errorString();
        return;
    }
    if (!json.isObject()) {
        qWarning() << "Loaded JSON must be an object, actual data type" << data;
        return; // TODO: set invalid state
    }
    m_data[level] = json.object();
    updateTree(level);
}

void ConfigEngine::setQmlEngine(QQmlEngine *qmlEngine)
{
    m_qmlEngine = qmlEngine;
    m_qmlEngine->rootContext()->setContextProperty("Config", m_root.object);
}

QObject *ConfigEngine::root() const
{
    return m_root.object;
}

void ConfigEngine::loadConfig(const QString &path, ConfigEngine::ConfigLevel level)
{
    qDebug() << "Loading config" << path;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        qWarning() << "File" << path << "not found!";
        return;
    }
    QByteArray data = f.readAll();
    f.close();
    loadData(data, level);
    qDebug() << "config loaded";
}

void ConfigEngine::updateTree(ConfigEngine::ConfigLevel level)
{
    if (level == Global) {
        // initial config
        m_root.clear();
        m_root.setJsonObject(m_data[level]);
        emit rootChanged();
        if (m_qmlEngine) {
            m_qmlEngine->rootContext()->setContextProperty("Config", m_root.object);
        }
    } else {
        // update with new properties
        m_root.updateJsonObject(m_data[level], level);
    }
}

const QVariant &ConfigEngine::Node::valueAt(int index) const
{
    return properties[index].value();
}

void ConfigEngine::Node::createObject()
{
    QMetaObjectBuilder b;
    QStringList classNameParts;
    Node *p = parent;
    if (!name.isEmpty()) {
        classNameParts.append(name);
        classNameParts.last()[0] = classNameParts.last()[0].toUpper();
    }
    while (p) {
        classNameParts.prepend(parent->name);
        classNameParts.first()[0] = classNameParts.first()[0].toUpper();
        p = p->parent;
    }
    if (!classNameParts.isEmpty()) {
        b.setClassName(classNameParts.join("").toLatin1());
    } else {
        b.setClassName("RootObject");
    }
    b.setSuperClass(&QObject::staticMetaObject);
    // add POD properties first
    for (auto it = properties.begin(); it != properties.end(); ++it) {
        QByteArray type;
        switch (it->values[Global].type()) {
        case QVariant::Bool:
            type = "bool";
            break;
        case QVariant::Double:
            type = "double";
            break;
        case QVariant::String:
            type = "QString";
            break;
        case QVariant::List:
            type = "QVariantList";
            break;
        case QVariant::LongLong:
            type = "qlonglong";
            break;
        default:
            qWarning() << "Unsupported property type" << it->key << it->values[Global].type();
            continue;
        }
        QByteArray pname = it->key.toLatin1();
        QMetaPropertyBuilder pb = b.addProperty(pname, type);
        pb.setStdCppSet(false);
        pb.setWritable(false);
        QByteArray sig(pname + "Changed()");
        QMetaMethodBuilder mb = b.addSignal(sig);
        mb.setReturnType("void");
        pb.setNotifySignal(mb);
    }

    for (auto it = childNodes.begin(); it != childNodes.end(); ++it) {
        QByteArray pname = (*it)->name.toLatin1();
        QMetaPropertyBuilder pb = b.addProperty(pname, "QObject*");
        QByteArray sig(pname + "Changed()");
        QMetaMethodBuilder mb = b.addSignal(sig);
        mb.setReturnType("void");
        pb.setNotifySignal(mb);
    }
    QMetaObject *mo = b.toMetaObject();
    if (object) {
        object->deleteLater();
    }

    object = new JSONQObject(mo, this, parent ? parent->object : nullptr);
}

void ConfigEngine::Node::setJsonObject(QJsonObject object)
{
    // split POD propertties from Object properties
    QMap<QString, QJsonObject> objects;
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (!it.value().isObject()) {
            properties.append(NamedValueGroup(it.key(), it.value().toVariant()));
        } else {
            objects[it.key()] = it.value().toObject();
        }
    }
    for (auto it = objects.begin(); it != objects.end(); ++it) {
        Node *n = new Node();
        n->name = it.key();
        childNodes.append(n);
        n->setJsonObject(it.value());
        n->parent = this;
    }
    createObject();
}

void ConfigEngine::Node::updateJsonObject(QJsonObject object, ConfigEngine::ConfigLevel level)
{

    QMap<QString, QJsonObject> objects;
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (!it.value().isObject()) {
            auto ii = std::find_if(properties.begin(), properties.end(), [&](const NamedValueGroup &v) { return v.key == it.key();});
            if (ii == properties.end()) {
                qWarning() << "Property" << fullPropertyName(it.key()) << "does not exist!";
                continue;
            }
            int id = std::distance(properties.begin(), ii);
            updateProperty(id, level, it.value().toVariant());
        } else {
            objects[it.key()] = it.value().toObject();
        }
    }
    for (auto it = objects.begin(); it != objects.end(); ++it) {
        auto ii = std::find_if(childNodes.begin(), childNodes.end(), [&](const Node *node) { return node->name == it.key();});
        if (ii == childNodes.end()) {
            qWarning() << "Property" << fullPropertyName(it.key()) << "does not exist!";
            continue;
        }
        (*ii)->updateJsonObject(it.value(), level);
    }
}

void ConfigEngine::Node::updateProperty(int index, ConfigEngine::ConfigLevel level, QVariant value)
{
    bool notify = true;
    properties[index].values[level] = value;

    for (int i = level + 1; i < LevelsCount; ++i) {
        notify &= !properties[index].values[i].isValid();
    }

    if (notify) {
        qDebug() << "Property" << fullPropertyName(properties[index].key) << "has changed";
        object->notifyPropertyUpdate(index);
    }
}

void ConfigEngine::Node::clear()
{
    for (auto child : childNodes) {
        child->clear();
    }

    qDeleteAll(childNodes);

    if (object) {
        object->deleteLater();
        object = nullptr;
    }
    properties.clear();
    name.clear();
}

QString ConfigEngine::Node::fullPropertyName(const QString &property) const
{
    QStringList pl;
    const Node *n = this;
    while (n) {
        if (!n->name.isEmpty()) {
            pl.prepend(n->name);
        }
        n = n->parent;
    }
    pl.append(property);
    return pl.join(".");
}

ConfigEngine::NamedValueGroup::NamedValueGroup(const QString &key, QVariant value)
    : key(key)
{
    values[Global] = value;
}

const QVariant &ConfigEngine::NamedValueGroup::value() const
{
    static const QVariant invalid;
    for(int i = LevelsCount - 1; i >= Global; --i) {
        if (values[i].isValid()) {
            return values[i];
        }
    }
    return invalid;
}
